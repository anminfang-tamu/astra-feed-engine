#pragma once

#include <cstdint>

enum class OrderAction : uint8_t {
  Add = 1,
  Modify = 2,
  Delete = 3,
  Trade = 4,
};