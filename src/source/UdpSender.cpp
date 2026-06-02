#include "astra/source/UdpSender.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

UdpSender::UdpSender(const char *ip, uint16_t port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0)
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

  dest_.sin_family = AF_INET;
  dest_.sin_port   = htons(port);
  if (::inet_pton(AF_INET, ip, &dest_.sin_addr) != 1)
    throw std::runtime_error(std::string("inet_pton: bad address: ") + ip);
}

UdpSender::~UdpSender() {
  if (fd_ >= 0)
    ::close(fd_);
}

bool UdpSender::send(const std::byte *data, std::size_t size) {
  ssize_t sent = ::sendto(fd_, data, size, 0,
                          reinterpret_cast<const struct sockaddr *>(&dest_),
                          sizeof(dest_));
  return sent == static_cast<ssize_t>(size);
}