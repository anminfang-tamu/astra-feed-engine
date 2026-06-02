#pragma once

#include "astra/source/IMarketDataSource.hpp"

#include <cstdint>

class ISnapshotSource : public IMarketDataSource {
public:
  ~ISnapshotSource() override = default;

  // Requests a full book snapshot for one symbol. min_seq_num lets callers ask
  // for a snapshot at or after a recovery boundary when the venue supports it.
  virtual bool requestSnapshot(uint32_t symbol_id, uint64_t min_seq_num) = 0;
};
