#pragma once

#include "astra/utils/FixedMmapArray.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

template <typename Value>
class FixedHashMap {
public:
  static_assert(std::is_trivially_copyable_v<Value>);

  explicit FixedHashMap(uint32_t capacity_pow2) noexcept
      : capacity_(checkedCapacity(capacity_pow2)), mask_(capacity_ - 1),
        entries_(capacity_) {}

  Value *find(uint64_t key) noexcept {
    const uint32_t slot = findSlot(key);
    return slot == kInvalidSlot ? nullptr : &entries_[slot].value;
  }

  const Value *find(uint64_t key) const noexcept {
    const uint32_t slot = findSlot(key);
    return slot == kInvalidSlot ? nullptr : &entries_[slot].value;
  }

  bool contains(uint64_t key) const noexcept { return find(key) != nullptr; }

  bool insert(uint64_t key, const Value &value) noexcept {
    if (size_ >= capacity_) return false;

    uint32_t slot = indexFor(key);
    for (uint32_t probes = 0; probes < capacity_; ++probes) {
      Entry &entry = entries_[slot];
      if (entry.state == kEmpty) {
        entry.key = key;
        entry.value = value;
        entry.state = kOccupied;
        ++size_;
        return true;
      }
      if (entry.key == key) return false;
      slot = nextSlot(slot);
    }
    return false;
  }

  bool insertOrAssign(uint64_t key, const Value &value) noexcept {
    if (Value *existing = find(key)) {
      *existing = value;
      return true;
    }
    return insert(key, value);
  }

  bool erase(uint64_t key) noexcept {
    const uint32_t slot = findSlot(key);
    if (slot == kInvalidSlot) return false;
    eraseAt(slot);
    --size_;
    return true;
  }

  void clear() noexcept {
    std::memset(entries_.data(), 0, entries_.size() * sizeof(Entry));
    size_ = 0;
  }

  uint32_t size() const noexcept { return size_; }
  uint32_t capacity() const noexcept { return capacity_; }

private:
  static constexpr uint8_t kEmpty = 0;
  static constexpr uint8_t kOccupied = 1;
  static constexpr uint32_t kInvalidSlot = std::numeric_limits<uint32_t>::max();

  struct Entry {
    uint64_t key;
    Value value;
    uint8_t state;
  };

  static uint32_t checkedCapacity(uint32_t capacity_pow2) noexcept {
    if (capacity_pow2 < 2 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
      assert(false && "FixedHashMap capacity must be a power of two >= 2");
      std::abort();
    }
    return capacity_pow2;
  }

  static uint64_t hash(uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
  }

  uint32_t indexFor(uint64_t key) const noexcept {
    return static_cast<uint32_t>(hash(key) & mask_);
  }

  uint32_t nextSlot(uint32_t slot) const noexcept { return (slot + 1) & mask_; }

  uint32_t probeDistance(uint32_t natural_slot, uint32_t slot) const noexcept {
    return (slot - natural_slot) & mask_;
  }

  uint32_t findSlot(uint64_t key) const noexcept {
    uint32_t slot = indexFor(key);
    for (uint32_t probes = 0; probes < capacity_; ++probes) {
      const Entry &entry = entries_[slot];
      if (entry.state == kEmpty) return kInvalidSlot;
      if (entry.key == key) return slot;
      slot = nextSlot(slot);
    }
    return kInvalidSlot;
  }

  void eraseAt(uint32_t erase_slot) noexcept {
    uint32_t gap = erase_slot;
    uint32_t slot = nextSlot(gap);

    for (uint32_t scanned = 0; scanned + 1 < capacity_; ++scanned) {
      Entry &entry = entries_[slot];
      if (entry.state == kEmpty) break;

      const uint32_t natural_slot = indexFor(entry.key);
      if (probeDistance(natural_slot, gap) <
          probeDistance(natural_slot, slot)) {
        entries_[gap] = entry;
        gap = slot;
      }
      slot = nextSlot(slot);
    }

    entries_[gap] = Entry{};
  }

  uint32_t capacity_{0};
  uint32_t mask_{0};
  uint32_t size_{0};
  FixedMmapArray<Entry> entries_;
};
