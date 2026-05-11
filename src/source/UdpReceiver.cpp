#include "astra/source/UdpReceiver.hpp"
#include "astra/core/Time.hpp"
#include "astra/source/PacketView.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

inline bool is_multicast(const struct in_addr &addr) {
  // 224.0.0.0/4 — top nibble of host-order address == 0xE
  return (ntohl(addr.s_addr) >> 28) == 0xEu;
}

} // namespace

UdpReceiver::UdpReceiver(const char *ip, uint16_t port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0)
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

  // Non-blocking: recv returns EAGAIN
  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0)
    throw std::runtime_error(std::string("O_NONBLOCK: ") +
                             std::strerror(errno));

  // Allow multiple sockets to bind the same port (required for multicast feeds)
  int yes = 1;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    throw std::runtime_error(std::string("SO_REUSEADDR: ") +
                             std::strerror(errno));

  // Large kernel receive buffer
  int rcvbuf = 8 * 1024 * 1024;
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
    throw std::runtime_error(std::string("SO_RCVBUF: ") + std::strerror(errno));

#ifdef SO_BUSY_POLL
  // Linux only: kernel busy-polls the NIC for up to N microseconds,
  // cutting interrupt round-trip latency without going full DPDK
  int busy_us = 50;
  ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif

  // Bind to INADDR_ANY so the kernel delivers both multicast and unicast
  struct sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  local.sin_addr.s_addr = INADDR_ANY;
  if (::bind(fd_, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) <
      0)
    throw std::runtime_error(std::string("bind: ") + std::strerror(errno));

  // Join multicast group if the feed address is in 224.0.0.0/4
  struct in_addr group{};
  if (::inet_pton(AF_INET, ip, &group) != 1)
    throw std::runtime_error(std::string("inet_pton: bad address: ") + ip);

  if (is_multicast(group)) {
    struct ip_mreq mreq{};
    mreq.imr_multiaddr = group;
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
        0)
      throw std::runtime_error(std::string("IP_ADD_MEMBERSHIP: ") +
                               std::strerror(errno));
  }
}

UdpReceiver::~UdpReceiver() {
  if (fd_ >= 0)
    ::close(fd_);
}

bool UdpReceiver::next(PacketView &packet) {
  ssize_t n = ::recv(fd_, buf_, sizeof(buf_), 0);
  if (n < 0)
    return false; // EAGAIN/EWOULDBLOCK: no data; EINTR: signal interrupted recv

  packet.data = buf_;
  packet.size = static_cast<std::size_t>(n);
  packet.receive_ts_ns = nowNs();
  return true;
}
