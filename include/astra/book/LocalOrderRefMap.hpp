#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Fixed-capacity order-reference index owned by one OrderBook.
//
// The map stores the full non-zero ITCH order reference and resolves it to an
// index in that book's local order pool. Construction allocates and pre-touches
// all storage. Insert, find, erase, and replaceKey never allocate or resize.
// The map is intentionally single-writer.
class LocalOrderRefMap {
public:
  static constexpr uint32_t kInvalidIndex =
      std::numeric_limits<uint32_t>::max();

  enum class InsertResult : uint8_t {
    Inserted,
    Duplicate,
    Full,
    Invalid,
  };

  enum class ReplaceResult : uint8_t {
    Replaced,
    NotFound,
    Duplicate,
    Invalid,
  };

  struct FailureCounters {
    uint64_t invalid_inserts{0};
    uint64_t duplicate_inserts{0};
    uint64_t full_inserts{0};
    uint64_t invalid_erases{0};
    uint64_t erase_misses{0};
    uint64_t invalid_replacements{0};
    uint64_t missing_replacements{0};
    uint64_t duplicate_replacements{0};

    friend constexpr bool operator==(FailureCounters,
                                     FailureCounters) noexcept = default;
  };

  // Snapshot of the current table layout. It is computed only when requested,
  // so normal insert/find/erase operations do not update telemetry counters in
  // the hot path.
  struct ProbeStats {
    uint64_t searches{0};
    uint64_t slots_examined{0};
    size_t max_length{0};

    double averageLength() const noexcept {
      return searches == 0
                 ? 0.0
                 : static_cast<double>(slots_examined) /
                       static_cast<double>(searches);
    }

    friend constexpr bool operator==(ProbeStats, ProbeStats) noexcept = default;
  };

  explicit LocalOrderRefMap(size_t slot_capacity)
      : entries_(validateSlotCapacity(slot_capacity)) {
    touchPages();
  }

  LocalOrderRefMap(const LocalOrderRefMap &) = delete;
  LocalOrderRefMap &operator=(const LocalOrderRefMap &) = delete;
  LocalOrderRefMap(LocalOrderRefMap &&) noexcept = default;
  LocalOrderRefMap &operator=(LocalOrderRefMap &&) noexcept = default;

  constexpr bool canStore(uint64_t order_ref) const noexcept {
    return order_ref != kEmptyKey;
  }

  InsertResult insert(uint64_t order_ref, uint32_t pool_index) noexcept {
    if (!canStore(order_ref) || pool_index == kInvalidIndex) {
      ++failures_.invalid_inserts;
      return InsertResult::Invalid;
    }

    const ProbeResult probe = probeFor(order_ref);
    if (probe.found) {
      ++failures_.duplicate_inserts;
      return InsertResult::Duplicate;
    }
    if (size_ >= maxEntries()) {
      ++failures_.full_inserts;
      return InsertResult::Full;
    }

    entries_[probe.index] = Entry{order_ref, pool_index, 0};
    ++size_;
    return InsertResult::Inserted;
  }

  uint32_t find(uint64_t order_ref) const noexcept {
    if (!canStore(order_ref)) {
      return kInvalidIndex;
    }

    const ProbeResult probe = probeFor(order_ref);
    return probe.found ? entries_[probe.index].pool_index : kInvalidIndex;
  }

  bool contains(uint64_t order_ref) const noexcept {
    return find(order_ref) != kInvalidIndex;
  }

  bool erase(uint64_t order_ref) noexcept {
    if (!canStore(order_ref)) {
      ++failures_.invalid_erases;
      return false;
    }

    const ProbeResult probe = probeFor(order_ref);
    if (!probe.found) {
      ++failures_.erase_misses;
      return false;
    }

    eraseAt(probe.index);
    --size_;
    return true;
  }

  // Changes an order reference while preserving its local pool index. The
  // operation remains available at maxEntries() because occupancy is unchanged.
  ReplaceResult replaceKey(uint64_t old_order_ref,
                           uint64_t new_order_ref) noexcept {
    if (!canStore(old_order_ref) || !canStore(new_order_ref)) {
      ++failures_.invalid_replacements;
      return ReplaceResult::Invalid;
    }

    const ProbeResult old_probe = probeFor(old_order_ref);
    if (!old_probe.found) {
      ++failures_.missing_replacements;
      return ReplaceResult::NotFound;
    }
    if (old_order_ref == new_order_ref) {
      return ReplaceResult::Replaced;
    }

    const ProbeResult new_probe = probeFor(new_order_ref);
    if (new_probe.found) {
      ++failures_.duplicate_replacements;
      return ReplaceResult::Duplicate;
    }

    const uint32_t pool_index = entries_[old_probe.index].pool_index;
    eraseAt(old_probe.index);
    --size_;

    // eraseAt() can move entries, so the new key's insertion slot must be
    // resolved again after the probe chain changes.
    const ProbeResult insert_probe = probeFor(new_order_ref);
    entries_[insert_probe.index] = Entry{new_order_ref, pool_index, 0};
    ++size_;
    return ReplaceResult::Replaced;
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return entries_.size(); }
  size_t maxEntries() const noexcept { return capacity() / 2; }
  bool empty() const noexcept { return size_ == 0; }

  double loadFactor() const noexcept {
    return static_cast<double>(size_) / static_cast<double>(capacity());
  }

  const FailureCounters &failureCounters() const noexcept { return failures_; }
  ProbeStats probeStats() const noexcept {
    ProbeStats result{};
    for (size_t slot = 0; slot < capacity(); ++slot) {
      const Entry &entry = entries_[slot];
      if (entry.key == kEmptyKey) {
        continue;
      }
      const size_t length = probeDistance(homeIndex(entry.key), slot) + 1;
      ++result.searches;
      result.slots_examined += length;
      if (length > result.max_length) {
        result.max_length = length;
      }
    }
    return result;
  }

private:
  static constexpr uint64_t kEmptyKey = 0;

  struct Entry {
    uint64_t key{0};
    uint32_t pool_index{kInvalidIndex};
    uint32_t reserved{0};
  };

  static_assert(sizeof(Entry) == 16,
                "LocalOrderRefMap entries must remain cache compact");

  struct ProbeResult {
    size_t index{0};
    size_t length{0};
    bool found{false};
  };

  static size_t validateSlotCapacity(size_t slot_capacity) {
    if (slot_capacity < 2 || !std::has_single_bit(slot_capacity)) {
      throw std::invalid_argument(
          "LocalOrderRefMap slot capacity must be a power of two >= 2");
    }
    return slot_capacity;
  }

  // SplitMix64's finalizer gives stable avalanche for sparse and sequential
  // exchange-assigned order references.
  static constexpr uint64_t hash(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  size_t homeIndex(uint64_t order_ref) const noexcept {
    return static_cast<size_t>(hash(order_ref)) & (capacity() - 1);
  }

  ProbeResult probeFor(uint64_t order_ref) const noexcept {
    size_t index = homeIndex(order_ref);
    for (size_t length = 1; length <= capacity(); ++length) {
      const uint64_t key = entries_[index].key;
      if (key == kEmptyKey) {
        return {index, length, false};
      }
      if (key == order_ref) {
        return {index, length, true};
      }
      index = (index + 1) & (capacity() - 1);
    }

    // The half-load invariant guarantees an empty slot in valid state.
    return {index, capacity(), false};
  }

  size_t probeDistance(size_t home, size_t slot) const noexcept {
    return (slot - home) & (capacity() - 1);
  }

  void eraseAt(size_t hole) noexcept {
    // Compact only the affected probe cluster. These are 16-byte Entry
    // assignments inside the already allocated vector; no heap allocation or
    // resize is possible here. Keeping a real Empty terminator avoids the
    // full-table probe degradation that permanent tombstones develop under a
    // full day of add/delete churn.
    size_t scan = (hole + 1) & (capacity() - 1);
    while (entries_[scan].key != kEmptyKey) {
      const size_t home = homeIndex(entries_[scan].key);
      if (probeDistance(home, hole) < probeDistance(home, scan)) {
        entries_[hole] = entries_[scan];
        hole = scan;
      }
      scan = (scan + 1) & (capacity() - 1);
    }
    entries_[hole] = {};
  }

  void touchPages() noexcept {
    static constexpr size_t kPageSize = 4096;
    const size_t byte_count = entries_.size() * sizeof(Entry);
    volatile uint8_t *bytes =
        reinterpret_cast<volatile uint8_t *>(entries_.data());
    for (size_t offset = 0; offset < byte_count; offset += kPageSize) {
      bytes[offset] = 0;
    }
    bytes[byte_count - 1] = 0;
  }

  std::vector<Entry> entries_;
  size_t size_{0};
  FailureCounters failures_{};
};
