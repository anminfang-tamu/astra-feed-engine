#pragma once

#include <array>
#include <cstdint>

struct BookLevelUpdate {
  uint64_t price;
  uint64_t qty;
  uint32_t num_orders;
};

struct BookUpdate {
  std::array<BookLevelUpdate, 10> bids; // top 10 bid levels
  std::array<BookLevelUpdate, 10> asks; // top 10 ask levels
  uint64_t timestamp;              // 8 bytes, optional
  uint32_t symbol_id;              // 4 bytes
  uint8_t bids_depth;              // 1 byte, actual number of bid levels (0-10)
  uint8_t asks_depth;              // 1 byte, actual number of ask levels (0-10)
};
