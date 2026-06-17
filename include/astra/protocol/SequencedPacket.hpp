#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct SequencedPacket {
  static constexpr std::size_t kMaxPacketSize = 2048;

  uint64_t seq{0};
  uint64_t receive_start_ticks{0};
  uint16_t msg_count{0};
  uint16_t len{0};
  std::array<std::byte, kMaxPacketSize> data{};
};
