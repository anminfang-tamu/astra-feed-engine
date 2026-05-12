#pragma once

#include "astra/protocol/OrderSide.hpp"
#include <cstdint>

struct Order {
  uint64_t order_id;  // 8 bytes
  uint32_t symbol_id; // 4 bytes
  uint32_t pool_index;
  uint64_t price;     // 8 bytes, fixed-point ticks
  uint32_t qty;       // 4 bytes
  OrderSide side;     // 1 byte // '1' for buy, '2' for sell
  bool active;

  Order *prev;
  Order *next;
};
