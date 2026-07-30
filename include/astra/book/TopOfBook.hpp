#pragma once

#include <cstdint>

struct TopOfBook {
  // Exchange timestamp from the latest successful mutation of this book.
  uint64_t timestamp;
  uint64_t bid_price; // best (highest) buy price
  uint64_t bid_qty;   // aggregate quantity can exceed one wire-order quantity
  uint64_t ask_price; // best (lowest) sell price
  uint64_t ask_qty;
  uint32_t symbol_id;
  // Raw ITCH price zero is valid, so price==0 cannot also mean "no side".
  bool has_bid;
  bool has_ask;
};
