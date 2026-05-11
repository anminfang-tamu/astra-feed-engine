#pragma once

#include "astra/sequencing/GapEvent.hpp"
#include "astra/source/IMarketDataSource.hpp"

class IReplaySource : public IMarketDataSource {
public:
  ~IReplaySource() override = default;

  // Requests the missing sequenced packets. Implementations should return them
  // from next() in sequence order.
  virtual bool requestGapFill(const GapEvent &gap) = 0;
};
