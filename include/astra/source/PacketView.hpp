#pragma once

#include <cstddef>
#include <cstdint>

struct PacketView {                // 24 bytes
  const std::byte *data{nullptr};  // 8 bytes
  std::size_t size{0};             // 8 bytes
  uint64_t receive_start_ticks{0}; // 8 bytes, rdtsc ticks
};
