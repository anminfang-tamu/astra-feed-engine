#pragma once

#include "astra/sequencing/GapEvent.hpp"
#include <cstdint>
class SequenceTracker {
public:
  explicit SequenceTracker(uint64_t start_seq = 1)
      : expected_seq_(start_seq), last_gap_{0, 0} {}

  bool onMessage(uint64_t seq_num);

  bool hasGap() const;
  GapEvent lastGap() const;

private:
  uint64_t expected_seq_;
  GapEvent last_gap_;
};
