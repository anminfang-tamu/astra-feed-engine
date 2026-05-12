#pragma once

#include <cstdint>

#include "MessageType.hpp"
#include "OrderSide.hpp"

struct MarketDataMessage { // 48 bytes with padding
  uint64_t seq_num;        // 8 bytes
  uint64_t exhange_ts_ns;  // 8 bytes
  uint64_t order_id;       // 8 bytes
  uint32_t symbol_id;      // 4 bytes
  uint64_t price;          // 8 bytes, fixed-point ticks
  uint32_t qty;            // 4 bytes
  MessageType type;        // 1 byte
  OrderSide side;          // 1 byte
};
