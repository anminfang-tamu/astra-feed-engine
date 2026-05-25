#include "astra/source/UdpBachReceiver.hpp"

#include "astra/core/Time.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

#ifdef __linux__
inline bool is_multicast(const struct in_addr &addr) noexcept {
  return (ntohl(addr.s_addr) >> 28) == 0xEu;
}

[[noreturn]] void throw_errno(const char *what) {
  throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}
#endif

} // namespace

UdpBachReceiver::UdpBachReceiver(const char *ip, uint16_t port,
                                 std::size_t batch_size)
    : batch_size_(std::clamp(batch_size, std::size_t{1}, kMaxBatchSize)) {
#ifndef __linux__
  (void)ip;
  (void)port;
  throw std::runtime_error("UdpBachReceiver requires Linux recvmmsg()");
#else
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0)
    throw_errno("socket");

  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0)
    throw_errno("O_NONBLOCK");

  int yes = 1;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    throw_errno("SO_REUSEADDR");

  int rcvbuf = 8 * 1024 * 1024;
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
    throw_errno("SO_RCVBUF");

#ifdef SO_BUSY_POLL
  int busy_us = 100;
  ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif

#ifdef SO_PREFER_BUSY_POLL
  int prefer = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer, sizeof(prefer));
#endif

#ifdef SO_INCOMING_CPU
  int cpu = ::sched_getcpu();
  if (cpu >= 0)
    ::setsockopt(fd_, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu));
#endif

#ifdef SO_RXQ_OVFL
  int overflow = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &overflow, sizeof(overflow));
#endif

  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  local.sin_addr.s_addr = INADDR_ANY;
  if (::bind(fd_, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) <
      0)
    throw_errno("bind");

  struct in_addr group {};
  if (::inet_pton(AF_INET, ip, &group) != 1) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(std::string("inet_pton: bad address: ") + ip);
  }

  if (is_multicast(group)) {
    struct ip_mreq mreq {};
    mreq.imr_multiaddr = group;
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                     sizeof(mreq)) < 0)
      throw_errno("IP_ADD_MEMBERSHIP");
  }

  for (std::size_t i = 0; i < batch_size_; ++i) {
    iovecs_[i].iov_base = buffers_[i].data();
    iovecs_[i].iov_len = buffers_[i].size();
    msgs_[i].msg_hdr.msg_iov = &iovecs_[i];
    msgs_[i].msg_hdr.msg_iovlen = 1;
    msgs_[i].msg_hdr.msg_control = controls_[i].data();
    msgs_[i].msg_hdr.msg_controllen = controls_[i].size();
  }
#endif
}

UdpBachReceiver::~UdpBachReceiver() {
  if (fd_ >= 0)
    ::close(fd_);
}

bool UdpBachReceiver::next(PacketView &packet) noexcept {
  if (pending_index_ >= pending_count_) {
    if (receiveBatch() == 0)
      return false;
  }

  packet = pending_[pending_index_++];
  return true;
}

std::size_t UdpBachReceiver::nextBatch(PacketView *packets,
                                       std::size_t max_packets) noexcept {
  if (packets == nullptr || max_packets == 0)
    return 0;

  std::size_t copied = 0;
  while (copied < max_packets) {
    if (pending_index_ >= pending_count_) {
      if (copied > 0 || receiveBatch() == 0)
        break;
    }

    while (copied < max_packets && pending_index_ < pending_count_)
      packets[copied++] = pending_[pending_index_++];
  }

  return copied;
}

std::size_t UdpBachReceiver::receiveBatch() noexcept {
  pending_index_ = 0;
  pending_count_ = 0;

#ifndef __linux__
  ++error_count_;
  return 0;
#else
  for (std::size_t i = 0; i < batch_size_; ++i) {
    msgs_[i].msg_len = 0;
    msgs_[i].msg_hdr.msg_flags = 0;
    msgs_[i].msg_hdr.msg_controllen = controls_[i].size();
  }

  const int n =
      ::recvmmsg(fd_, msgs_.data(), static_cast<unsigned int>(batch_size_),
                 MSG_DONTWAIT | MSG_TRUNC, nullptr);

  if (n < 0) [[unlikely]] {
    const int e = errno;
    if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR)
      ++error_count_;
    return 0;
  }

  ++syscall_count_;
  const uint64_t receive_ts_ns = nowNs();
  const std::size_t count = static_cast<std::size_t>(n);

  for (std::size_t i = 0; i < count; ++i) {
    recordKernelDrops(msgs_[i].msg_hdr);

    std::size_t size = msgs_[i].msg_len;
    if (size > kMaxPacketSize) [[unlikely]] {
      ++truncated_count_;
      size = kMaxPacketSize;
    }

    pending_[i] = PacketView{buffers_[i].data(), size, receive_ts_ns};
  }

  pending_count_ = count;
  return count;
#endif
}

#ifdef __linux__
void UdpBachReceiver::recordKernelDrops(const struct msghdr &msg) noexcept {
#ifdef SO_RXQ_OVFL
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(const_cast<struct msghdr *>(&msg), cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SO_RXQ_OVFL)
      continue;

    uint32_t kernel_drop_count = 0;
    std::memcpy(&kernel_drop_count, CMSG_DATA(cmsg),
                sizeof(kernel_drop_count));
    if (kernel_drop_count >= last_kernel_drop_count_) {
      kernel_drop_count_ += kernel_drop_count - last_kernel_drop_count_;
    } else {
      kernel_drop_count_ +=
          static_cast<uint64_t>(UINT_MAX - last_kernel_drop_count_) +
          kernel_drop_count + 1u;
    }
    last_kernel_drop_count_ = kernel_drop_count;
    return;
  }
#else
  (void)msg;
#endif
}
#endif
