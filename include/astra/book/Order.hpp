#pragma once

#include <cstdint>

struct Order {
  static constexpr uint32_t kSideMask = uint32_t{1} << 31;
  static constexpr uint32_t kLevelMask = ~kSideMask;
  static constexpr uint32_t kInvalidLevel = 0;

  uint64_t order_id;
  uint64_t price;
  uint32_t prev_idx;
  uint32_t next_idx;
  uint32_t qty;
  uint32_t level_and_side;

  uint32_t levelIndex() const noexcept { return level_and_side & kLevelMask; }

  char side() const noexcept {
    return (level_and_side & kSideMask) == 0 ? 'B' : 'S';
  }

  void setLevelAndSide(uint32_t level_idx, char order_side) noexcept {
    level_and_side = (level_idx & kLevelMask) |
                     (order_side == 'S' ? kSideMask : uint32_t{0});
  }
};

static_assert(sizeof(Order) == 32,
              "Order must remain two records per 64-byte cache line");
