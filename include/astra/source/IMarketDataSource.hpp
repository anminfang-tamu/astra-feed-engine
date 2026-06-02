#pragma once

#include "PacketView.hpp"

class IMarketDataSource {
public:
  virtual ~IMarketDataSource() = default;

  virtual bool next(PacketView &packet) = 0;
};