#pragma once

#include "IMarketDataSource.hpp"

#include <cstdint>

class DpdkReceiver : public IMarketDataSource {
public:
  DpdkReceiver(const char *ip, uint16_t port);
  virtual bool next(PacketView &packet) override;
};