#pragma once

#include <cstddef>
#include <cstdint>

#include "MessageType.hpp"
#include "OrderSide.hpp"

#pragma pack(push, 1)
struct WireMessage {      // 38 bytes
  uint64_t seq_num;       // 8 bytes
  uint64_t exhange_ts_ns; // 8 bytes
  uint64_t order_id;      // 8 bytes
  uint32_t symbol_id;     // 4 bytes
  uint32_t price;         // 4 bytes
  uint32_t qty;           // 4 bytes
  MessageType type;       // 1 byte
  OrderSide side;         // 1 byte
};
#pragma pack(pop)

constexpr std::size_t WireMessageSize = 38;
static_assert(sizeof(WireMessage) == WireMessageSize,
              "WireMessage layout changed");
