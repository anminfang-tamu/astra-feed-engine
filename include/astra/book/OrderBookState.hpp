#pragma once

#include <cstdint>

enum class OrderBookState : uint8_t {
  Normal = 1,
  Stale = 2,
  Recovering = 3,
};
