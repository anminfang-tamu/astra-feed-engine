#pragma once

#include <cstdint>
#include <limits>
#include <vector>

class Bitmap {
public:
  Bitmap() = default;

  void set(uint32_t from_index);
  void reset(uint32_t from_index);
  uint32_t findPrevSet(uint32_t index) const;
  uint32_t findNextSet(uint32_t index) const;

private:
  uint64_t maskThroughBit(uint32_t bit_index) const;

  static constexpr uint32_t kBitmapWordBits = 64;
  static constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();
  std::vector<uint64_t> words_;
};
