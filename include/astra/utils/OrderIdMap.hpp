#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

class OrderIdMap {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();

private:
  static constexpr uint8_t kEmpty = 0;
  static constexpr uint8_t kOccupied = 1;

  struct Entry {
    uint64_t order_ref{0};
    uint32_t pool_index{kInvalidIdx};
    uint8_t state{kEmpty};
  };

public:
  explicit OrderIdMap(uint32_t capacity_pow2)
      : entries_(checkedCapacity(capacity_pow2)), capacity_(capacity_pow2),
        mask_(capacity_pow2 - 1) {}

  OrderIdMap(const OrderIdMap &) = delete;
  OrderIdMap &operator=(const OrderIdMap &) = delete;

  OrderIdMap(OrderIdMap &&other) noexcept { moveFrom(other); }

  OrderIdMap &operator=(OrderIdMap &&other) noexcept {
    if (this != &other) {
      entries_ = std::move(other.entries_);
      capacity_ = other.capacity_;
      mask_ = other.mask_;
      size_ = other.size_;

      other.capacity_ = 0;
      other.mask_ = 0;
      other.size_ = 0;
    }

    return *this;
  }

  bool insert(uint64_t itch_order_ref, uint32_t pool_index) noexcept {
    if (pool_index == kInvalidIdx || size_ >= capacity_) {
      return false;
    }

    uint32_t slot = indexFor(itch_order_ref);
    for (uint32_t probes = 0; probes < capacity_; ++probes) {
      Entry &entry = entries_[slot];

      if (entry.state == kEmpty) {
        entry.order_ref = itch_order_ref;
        entry.pool_index = pool_index;
        entry.state = kOccupied;
        ++size_;
        return true;
      }

      if (entry.order_ref == itch_order_ref) {
        return false;
      }

      slot = nextSlot(slot);
    }

    return false;
  }

  uint32_t find(uint64_t itch_order_ref) const noexcept {
    uint32_t slot = indexFor(itch_order_ref);
    for (uint32_t probes = 0; probes < capacity_; ++probes) {
      const Entry &entry = entries_[slot];

      if (entry.state == kEmpty) {
        return kInvalidIdx;
      }

      if (entry.order_ref == itch_order_ref) {
        return entry.pool_index;
      }

      slot = nextSlot(slot);
    }

    return kInvalidIdx;
  }

  bool erase(uint64_t itch_order_ref) noexcept {
    uint32_t slot = indexFor(itch_order_ref);
    for (uint32_t probes = 0; probes < capacity_; ++probes) {
      const Entry &entry = entries_[slot];

      if (entry.state == kEmpty) {
        return false;
      }

      if (entry.order_ref == itch_order_ref) {
        eraseAt(slot);
        --size_;
        return true;
      }

      slot = nextSlot(slot);
    }

    return false;
  }

  bool contains(uint64_t itch_order_ref) const noexcept {
    return find(itch_order_ref) != kInvalidIdx;
  }

  void clear() noexcept {
    for (Entry &entry : entries_) {
      entry.order_ref = 0;
      entry.pool_index = kInvalidIdx;
      entry.state = kEmpty;
    }

    size_ = 0;
  }

  uint32_t size() const noexcept { return size_; }

  uint32_t capacity() const noexcept { return capacity_; }

  bool empty() const noexcept { return size_ == 0; }

  double loadFactor() const noexcept {
    if (capacity_ == 0) {
      return 0.0;
    }

    return static_cast<double>(size_) / static_cast<double>(capacity_);
  }

  uint32_t maxProbeLength() const noexcept {
    uint32_t max_probe_length = 0;

    for (uint32_t slot = 0; slot < capacity_; ++slot) {
      const Entry &entry = entries_[slot];
      if (entry.state != kOccupied) {
        continue;
      }

      const uint32_t probe_length =
          probeDistance(indexFor(entry.order_ref), slot) + 1;
      if (probe_length > max_probe_length) {
        max_probe_length = probe_length;
      }
    }

    return max_probe_length;
  }

  double averageProbeLength() const noexcept {
    if (size_ == 0) {
      return 0.0;
    }

    uint64_t total_probe_length = 0;
    for (uint32_t slot = 0; slot < capacity_; ++slot) {
      const Entry &entry = entries_[slot];
      if (entry.state != kOccupied) {
        continue;
      }

      total_probe_length += probeDistance(indexFor(entry.order_ref), slot) + 1;
    }

    return static_cast<double>(total_probe_length) / static_cast<double>(size_);
  }

private:
  static uint32_t checkedCapacity(uint32_t capacity_pow2) noexcept {
    if (capacity_pow2 < 2 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
      assert(false && "OrderIndex capacity must be a power of two >= 2");
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

  uint32_t indexFor(uint64_t itch_order_ref) const noexcept {
    return static_cast<uint32_t>(hash(itch_order_ref) & mask_);
  }

  uint32_t nextSlot(uint32_t slot) const noexcept { return (slot + 1) & mask_; }

  uint32_t probeDistance(uint32_t natural_slot, uint32_t slot) const noexcept {
    return (slot - natural_slot) & mask_;
  }

  void eraseAt(uint32_t erase_slot) noexcept {
    uint32_t gap = erase_slot;
    uint32_t slot = nextSlot(gap);

    for (uint32_t scanned = 0; scanned + 1 < capacity_; ++scanned) {
      Entry &entry = entries_[slot];
      if (entry.state == kEmpty) {
        break;
      }

      const uint32_t natural_slot = indexFor(entry.order_ref);
      if (probeDistance(natural_slot, gap) <
          probeDistance(natural_slot, slot)) {
        entries_[gap] = entry;
        gap = slot;
      }

      slot = nextSlot(slot);
    }

    entries_[gap].order_ref = 0;
    entries_[gap].pool_index = kInvalidIdx;
    entries_[gap].state = kEmpty;
  }

  void moveFrom(OrderIdMap &other) noexcept {
    entries_ = std::move(other.entries_);
    capacity_ = other.capacity_;
    mask_ = other.mask_;
    size_ = other.size_;

    other.capacity_ = 0;
    other.mask_ = 0;
    other.size_ = 0;
  }

  std::vector<Entry> entries_;
  uint32_t capacity_{0};
  uint32_t mask_{0};
  uint32_t size_{0};
};
