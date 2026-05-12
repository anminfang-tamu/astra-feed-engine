#pragma once

#include "MdEventType.hpp"

#include <cstdint>

struct MdEvent {
  uint64_t ts_event_ns;
  uint64_t channel_seq_no;
  uint64_t symbol_seq_no;
  uint32_t symbol_id;
  MdEventType type;
  char side; // B/S where available; otherwise derive from book
  uint64_t order_id;
  uint64_t new_order_id; // only for Replace
  int64_t price_ticks;
  uint32_t qty;
  uint8_t channel_id;
};