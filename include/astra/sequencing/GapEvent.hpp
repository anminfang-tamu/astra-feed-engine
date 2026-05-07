#pragma once

#include <cstddef>
#include <cstdint>

struct GapEvent {         // 16 bytes
  uint64_t first_missing; // 8 bytes
  uint64_t last_missing;  // 8 bytes
};