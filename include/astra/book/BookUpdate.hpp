#pragma once

#include <array>
#include <cstdint>

#include "PriceLevel.hpp"

struct BookUpdate {                // 256 bytes total
  std::array<PriceLevel, 10> bids; // 120 bytes, top 10 bid levels
  std::array<PriceLevel, 10> asks; // 120 bytes, top 10 ask levels
  uint64_t timestamp;              // 8 bytes, optional
  uint32_t symbol_id;              // 4 bytes
  uint8_t bids_depth;              // 1 byte, actual number of bid levels (0-10)
  uint8_t asks_depth;              // 1 byte, actual number of ask levels (0-10)
};