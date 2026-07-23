#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

struct SequencedPacket {
  static constexpr std::size_t kMaxPacketSize = 2048;

  // Keep this an implicit-lifetime/trivial record. Value-initialization with
  // SequencedPacket{} still produces the zero state where callers need it,
  // while FixedMmapArray can establish the complete packet-slot array without
  // invoking a constructor for every recovery slot.
  uint64_t seq;
  uint64_t receive_start_ticks;
  uint16_t msg_count;
  uint16_t len;
  std::array<std::byte, kMaxPacketSize> data;
};

static_assert(std::is_trivial_v<SequencedPacket>);
static_assert(std::is_trivially_copyable_v<SequencedPacket>);
