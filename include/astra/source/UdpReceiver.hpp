#pragma once

#include "IMarketDataSource.hpp"

#include <cstddef>
#include <cstdint>

class UdpReceiver : public IMarketDataSource {
public:
  UdpReceiver(const char *ip, uint16_t port);
  ~UdpReceiver();

  bool next(PacketView &packet) override;

private:
  static constexpr std::size_t kMaxPacketSize = 65536;

  int fd_{-1};
  alignas(64) std::byte buf_[kMaxPacketSize];
};