#pragma once

#include "astra/protocol/SequencedPacket.hpp"
#include "astra/utils/FixedMmapArray.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

class GapBuffer {
public:
  static constexpr uint32_t kCapacity = 1024 * 1024;

  struct Stats {
    uint32_t size{0};
    uint64_t min_seq{0};
    uint64_t max_seq{0};
    uint64_t next_seq_at_or_after{0};
  };

  GapBuffer() noexcept : slots_(kCapacity) { clear(); }

  GapBuffer(const GapBuffer &) = delete;
  GapBuffer &operator=(const GapBuffer &) = delete;
  GapBuffer(GapBuffer &&) = delete;
  GapBuffer &operator=(GapBuffer &&) = delete;

  bool insert(const std::byte *data, uint16_t len, uint64_t seq,
              uint16_t msg_count, uint64_t receive_start_ticks) noexcept {
    if (data == nullptr || len > SequencedPacket::kMaxPacketSize)
      return false;

    const uint32_t start = slotFor(seq);
    for (uint32_t probes = 0; probes < kCapacity; ++probes) {
      Slot &slot = slots_[(start + probes) & kMask];
      if (!slot.occupied || slot.packet.seq == seq) {
        if (!slot.occupied)
          ++size_;
        slot.occupied = true;
        slot.packet.seq = seq;
        slot.packet.receive_start_ticks = receive_start_ticks;
        slot.packet.msg_count = msg_count;
        slot.packet.len = len;
        std::memcpy(slot.packet.data.data(), data, len);
        return true;
      }
    }
    return false;
  }

  SequencedPacket *find(uint64_t seq) noexcept {
    const uint32_t start = slotFor(seq);
    for (uint32_t probes = 0; probes < kCapacity; ++probes) {
      Slot &slot = slots_[(start + probes) & kMask];
      if (!slot.occupied)
        return nullptr;
      if (slot.packet.seq == seq)
        return &slot.packet;
    }
    return nullptr;
  }

  void erase(uint64_t seq) noexcept {
    const uint32_t start = slotFor(seq);
    for (uint32_t probes = 0; probes < kCapacity; ++probes) {
      Slot &slot = slots_[(start + probes) & kMask];
      if (!slot.occupied)
        return;
      if (slot.packet.seq == seq) {
        eraseAt((start + probes) & kMask);
        return;
      }
    }
  }

  void clear() noexcept {
    for (uint32_t i = 0; i < kCapacity; ++i)
      slots_[i] = Slot{};
    size_ = 0;
  }
  bool empty() const noexcept { return size_ == 0; }
  uint32_t size() const noexcept { return size_; }

  Stats stats(uint64_t expected_seq) const noexcept {
    Stats stats;
    stats.size = size_;
    if (size_ == 0)
      return stats;

    uint64_t min_seq = std::numeric_limits<uint64_t>::max();
    uint64_t max_seq = 0;
    uint64_t next_seq = std::numeric_limits<uint64_t>::max();

    for (uint32_t i = 0; i < kCapacity; ++i) {
      const Slot &slot = slots_[i];
      if (!slot.occupied)
        continue;

      const uint64_t seq = slot.packet.seq;
      if (seq < min_seq)
        min_seq = seq;
      if (seq > max_seq)
        max_seq = seq;
      if (seq >= expected_seq && seq < next_seq)
        next_seq = seq;
    }

    stats.min_seq = min_seq;
    stats.max_seq = max_seq;
    if (next_seq != std::numeric_limits<uint64_t>::max())
      stats.next_seq_at_or_after = next_seq;
    return stats;
  }

private:
  static constexpr uint32_t kMask = kCapacity - 1;
  static_assert((kCapacity & kMask) == 0);

  struct Slot {
    bool occupied{false};
    SequencedPacket packet{};
  };

  static uint64_t hash(uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
  }

  static uint32_t slotFor(uint64_t seq) noexcept {
    return static_cast<uint32_t>(hash(seq) & kMask);
  }

  static uint32_t next(uint32_t slot) noexcept { return (slot + 1) & kMask; }

  static uint32_t probeDistance(uint32_t natural, uint32_t slot) noexcept {
    return (slot - natural) & kMask;
  }

  void eraseAt(uint32_t erase_slot) noexcept {
    uint32_t gap = erase_slot;
    uint32_t slot = next(gap);

    for (uint32_t scanned = 0; scanned + 1 < kCapacity; ++scanned) {
      Slot &entry = slots_[slot];
      if (!entry.occupied)
        break;

      const uint32_t natural = slotFor(entry.packet.seq);
      if (probeDistance(natural, gap) < probeDistance(natural, slot)) {
        slots_[gap] = entry;
        gap = slot;
      }
      slot = next(slot);
    }

    slots_[gap] = Slot{};
    --size_;
  }

  FixedMmapArray<Slot> slots_;
  uint32_t size_{0};
};
