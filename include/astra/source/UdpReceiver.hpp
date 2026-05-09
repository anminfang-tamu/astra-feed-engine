#pragma once

#include "IMarketDataSource.hpp"

#include <cstdint>

class UdpReceiver : public IMarketDataSource {
public:
  UdpReceiver(const char *ip, uint16_t port);
  virtual bool next(PacketView &packet) override;
};