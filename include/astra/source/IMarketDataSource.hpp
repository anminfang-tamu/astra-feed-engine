#pragma once

#include "PacketView.hpp"

class IMarketDataSource {
public:
  virtual ~IMarketDataSource() = default;

  // Throughput-only runs can disable receive timestamp capture entirely.
  // Sources without timestamping work may keep the default no-op.
  virtual void setTimestampingEnabled(bool enabled) noexcept {
    (void)enabled;
  }
  virtual bool next(PacketView &packet) = 0;
};
