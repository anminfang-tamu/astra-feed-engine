#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

class UdpSender {
public:
  UdpSender(const char *ip, uint16_t port);
  ~UdpSender();

  bool send(const std::byte *data, std::size_t size);

private:
  int fd_{-1};
  struct sockaddr_in dest_{};
};
