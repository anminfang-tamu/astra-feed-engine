#include "astra/source/UdpReceiver.hpp"
#include "astra/core/Time.hpp"
#include "astra/source/PacketView.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

inline bool is_multicast(const struct in_addr &addr) noexcept {
  // 224.0.0.0/4 — top nibble of host-order address == 0xE
  return (ntohl(addr.s_addr) >> 28) == 0xEu;
}

// Helper that throws a runtime_error with errno context.
[[noreturn]] void throw_errno(const char *what) {
  throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

} // namespace

UdpReceiver::UdpReceiver(const char *ip, uint16_t port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0)
    throw_errno("socket");

  // Non-blocking: recv returns EAGAIN. We also pass MSG_DONTWAIT per-call
  // for clarity, but socket-level O_NONBLOCK is the canonical control.
  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0)
    throw_errno("O_NONBLOCK");

  // Allow multiple sockets to bind the same port (required for multicast feeds
  // where multiple processes consume the same group on one host).
  int yes = 1;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    throw_errno("SO_REUSEADDR");

  // Large kernel receive buffer. 8 MB is enough headroom for brief consumer
  // stalls without dropping packets, small enough to stay cache-friendly.
  int rcvbuf = 8 * 1024 * 1024;
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
    throw_errno("SO_RCVBUF");

#ifdef SO_BUSY_POLL
  // Linux only: kernel busy-polls the NIC for up to N microseconds, cutting
  // interrupt round-trip latency without going full kernel-bypass.
  // 100us is a reasonable HFT value — higher values waste CPU during quiet
  // periods, lower values fall back to interrupt-driven delivery too easily.
  int busy_us = 100;
  ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif

#ifdef SO_PREFER_BUSY_POLL
  // Linux 5.11+: prefer busy-poll over softirq, further reducing scheduler
  // wakeups. Silently no-op on older kernels.
  int prefer = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer, sizeof(prefer));
#endif

#ifdef SO_INCOMING_CPU
  // Linux only: hint to the kernel that this socket should be steered to
  // the CPU the receive thread is currently on. Combined with thread pinning,
  // this keeps packets and the consumer on the same core / NUMA node.
  int cpu = ::sched_getcpu();
  if (cpu >= 0)
    ::setsockopt(fd_, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu));
#endif

  // Bind to INADDR_ANY so the kernel delivers both multicast and unicast.
  // Binding to the multicast IP itself is a common mistake — it works for
  // multicast but blocks any unicast traffic on the same port.
  struct sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  local.sin_addr.s_addr = INADDR_ANY;
  if (::bind(fd_, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) <
      0)
    throw_errno("bind");

  // Join multicast group if the feed address is in 224.0.0.0/4.
  struct in_addr group{};
  if (::inet_pton(AF_INET, ip, &group) != 1) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(std::string("inet_pton: bad address: ") + ip);
  }

  if (is_multicast(group)) {
    struct ip_mreq mreq{};
    mreq.imr_multiaddr = group;
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
        0)
      throw_errno("IP_ADD_MEMBERSHIP");
  }

  // Pre-fault the receive buffer so the first packet doesn't take a page
  // fault. Cheap insurance against tail-latency spikes on first use.
  std::memset(buf_, 0, sizeof(buf_));
}

UdpReceiver::~UdpReceiver() {
  if (fd_ >= 0)
    ::close(fd_);
}

bool UdpReceiver::next(PacketView &packet) noexcept {
  const uint64_t receive_start_ticks = rdtsc();

  // MSG_DONTWAIT — explicit non-blocking on this call (belt-and-braces with
  // O_NONBLOCK). MSG_TRUNC   — return the original packet length even if it
  // exceeded our buffer,
  //               so we can detect truncation rather than silently dropping
  //               bytes.
  const ssize_t n = ::recv(fd_, buf_, sizeof(buf_), MSG_DONTWAIT | MSG_TRUNC);

  if (n < 0) [[unlikely]] {
    // EAGAIN/EWOULDBLOCK: no data yet — normal in the busy-poll loop.
    // EINTR: signal interrupted recv — also benign; the caller will retry.
    // Anything else is a real error worth counting for diagnostics.
    const int e = errno;
    if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR) {
      ++error_count_;
    }
    return false;
  }

  std::size_t size = static_cast<std::size_t>(n);
  if (size > sizeof(buf_)) [[unlikely]] {
    // Packet was larger than our buffer; we got the truncated bytes.
    // Count it and clamp size to what we actually have.
    // ++truncated_count_;
    // size = sizeof(buf_);
    return false;
  }

  packet.data = buf_;
  packet.size = size;
  packet.receive_start_ticks = receive_start_ticks;
  return true;
}
