#pragma once

#include <cstdint>

struct TopOfBook {
  uint64_t timestamp; // 8 bytes, optional
  uint32_t symbol_id; // 4 bytes
  uint64_t bid_price; // 8 bytes, best (highest) buy price
  uint32_t bid_qty;   // 4 bytes, total qty at best bid
  uint64_t ask_price; // 8 bytes, best (lowest) sell price
  uint32_t ask_qty;   // 4 bytes, total qty at best ask
};
