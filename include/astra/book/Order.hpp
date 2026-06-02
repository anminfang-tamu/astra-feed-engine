#pragma once

#include "astra/protocol/OrderSide.hpp"
#include <cstdint>

struct Order {        // 32 bytes
  uint32_t prev_idx;  // 4 bytes — sentinel kInvalidIdx for head
  uint32_t next_idx;  // 4 bytes — sentinel kInvalidIdx for tail
  uint32_t qty;       // 4 bytes
  uint32_t level_idx; // 4 bytes — back-pointer to PriceLevel
  uint64_t price;     // 8 bytes — could drop if level_idx implies price,
  uint64_t order_id;  // 8 bytes — exchange ID, only read on ID-lookup miss path
  OrderSide side;     // 1 byte
  // 7 bytes padding
};
