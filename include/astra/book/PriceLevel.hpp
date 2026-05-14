#pragma once

#include <cstdint>

struct alignas(16) PriceLevel { // 16 bytes
  uint32_t head_idx;            // 4 bytes — first order  at this price level
  uint32_t tail_idx;            // 4 bytes — last order at this price level
  uint32_t total_qty;           // 4 bytes, total quantity at this price level
  uint32_t num_orders;          // 4 bytes, number of orders at this price level
};
