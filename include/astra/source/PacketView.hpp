#pragma once

#include <cstddef>
#include <cstdint>

struct PacketView {
  const std::byte *data{nullptr};
  std::size_t size{0};
  uint64_t receive_start_ticks{0}; // rdtsc ticks
  uint8_t line_index{0};           // 0 for single-line or line A, 1 for line B
};
