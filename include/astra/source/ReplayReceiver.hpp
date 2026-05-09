#pragma once

#include "IMarketDataSource.hpp"

class ReplayReceiver : public IMarketDataSource {
public:
  ReplayReceiver(const char *file_path);
  virtual bool next(PacketView &packet) override;
};