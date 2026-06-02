#include "astra/sequencing/SequenceTracker.hpp"

#include <cstdint>

SequenceStatus SequenceTracker::onMessage(uint64_t seq_num) {
  if (seq_num == expected_seq_) {
    expected_seq_++;
    return SequenceStatus::Ok;
  } else if (seq_num > expected_seq_) {
    last_gap_ = {expected_seq_, seq_num - 1};
    expected_seq_ = seq_num + 1;
    return SequenceStatus::Gap;
  } else {
    // Duplicate or out of order message, ignore
    return SequenceStatus::DuplicateOrOld;
  }
}

bool SequenceTracker::hasGap() const { return last_gap_.first_missing != 0; }

GapEvent SequenceTracker::lastGap() const { return last_gap_; }
