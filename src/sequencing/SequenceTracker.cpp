#include "astra/sequencing/SequenceTracker.hpp"

#include <cstdint>

bool SequenceTracker::onMessage(uint64_t seq_num) {
  if (seq_num == expected_seq_) {
    expected_seq_++;
    return true;
  } else if (seq_num > expected_seq_) {
    last_gap_ = {expected_seq_, seq_num - 1};
    expected_seq_ = seq_num + 1;
    return true;
  } else {
    // Duplicate or out of order message, ignore
    return false;
  }
}

bool SequenceTracker::hasGap() const { return last_gap_.first_missing != 0; }

GapEvent SequenceTracker::lastGap() const { return last_gap_; }
