#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Fixed-capacity, process-wide order-reference index.
//
// The directory is the authoritative mapping from the full 64-bit ITCH order
// reference space to an order pool slot. All storage is allocated and
// pre-touched by the constructor. Hot-path operations neither allocate nor
// resize the table.
class OrderRefDirectory {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();

  // 16,777,216 slots = 256 MiB of directory storage and at most 8,388,608
  // concurrently live orders. Deployments can select another power-of-two
  // slot count explicitly at construction.
  static constexpr size_t kDefaultSlotCapacity = size_t{1} << 24;
  static constexpr size_t kDefaultMaxEntries = kDefaultSlotCapacity / 2;

  struct Handle {
    uint16_t stock_locate{0};
    uint32_t pool_index{kInvalidIdx};

    constexpr bool valid() const noexcept {
      return stock_locate != 0 && pool_index != kInvalidIdx;
    }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;
  };

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
    uint64_t erase_misses{0};
    uint64_t invalid_replacements{0};
    uint64_t missing_replacements{0};
    uint64_t duplicate_replacements{0};

    friend constexpr bool operator==(FailureCounters,
                                     FailureCounters) noexcept = default;
  };

  explicit OrderRefDirectory(size_t slot_capacity = kDefaultSlotCapacity)
      : entries_(validateSlotCapacity(slot_capacity)) {
    touchPages();
  }

  OrderRefDirectory(const OrderRefDirectory &) = delete;
  OrderRefDirectory &operator=(const OrderRefDirectory &) = delete;
  OrderRefDirectory(OrderRefDirectory &&) noexcept = default;
  OrderRefDirectory &operator=(OrderRefDirectory &&) noexcept = default;

  // A non-zero uint64_t is representable regardless of its magnitude. This
  // predicate does not reserve capacity; callers must inspect insert().
  constexpr bool canStore(uint64_t order_ref) const noexcept {
    return order_ref != kEmptyKey;
  }

  InsertResult insert(uint64_t order_ref, uint16_t stock_locate,
                      uint32_t pool_index) noexcept {
    const Handle handle{stock_locate, pool_index};
    if (!canStore(order_ref) || !handle.valid()) {
      ++failures_.invalid_inserts;
      return InsertResult::Invalid;
    }

    const ProbeResult probe = probeFor(order_ref);
    observeProbeLength(probe.length);
    if (probe.found) {
      ++failures_.duplicate_inserts;
      return InsertResult::Duplicate;
    }
    if (size_ >= maxEntries()) {
      ++failures_.full_inserts;
      return InsertResult::Full;
    }

    entries_[probe.index] = Entry{order_ref, pack(handle)};
    ++size_;
    return InsertResult::Inserted;
  }

  Handle find(uint64_t order_ref) const noexcept {
    if (!canStore(order_ref)) {
      return {};
    }

    const ProbeResult probe = probeFor(order_ref);
    return probe.found ? unpack(entries_[probe.index].packed_handle) : Handle{};
  }

  bool erase(uint64_t order_ref) noexcept {
    if (!canStore(order_ref)) {
      ++failures_.erase_misses;
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

  // Atomically changes an order reference while preserving its Handle. This
  // remains possible at maxEntries() because the operation does not increase
  // table occupancy.
  ReplaceResult replaceKey(uint64_t old_order_ref,
                           uint64_t new_order_ref) noexcept {
    if (!canStore(old_order_ref) || !canStore(new_order_ref)) {
      ++failures_.invalid_replacements;
      return ReplaceResult::Invalid;
    }

    const ProbeResult old_probe = probeFor(old_order_ref);
    observeProbeLength(old_probe.length);
    if (!old_probe.found) {
      ++failures_.missing_replacements;
      return ReplaceResult::NotFound;
    }
    if (old_order_ref == new_order_ref) {
      return ReplaceResult::Replaced;
    }

    const ProbeResult new_probe = probeFor(new_order_ref);
    observeProbeLength(new_probe.length);
    if (new_probe.found) {
      ++failures_.duplicate_replacements;
      return ReplaceResult::Duplicate;
    }

    const uint64_t packed_handle = entries_[old_probe.index].packed_handle;
    eraseAt(old_probe.index);
    --size_;

    // eraseAt() can move entries, so locate the new key's insertion slot again.
    const ProbeResult insert_probe = probeFor(new_order_ref);
    observeProbeLength(insert_probe.length);
    entries_[insert_probe.index] = Entry{new_order_ref, packed_handle};
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

  // One means the key's home slot was examined; values above one indicate a
  // collision chain. This is the maximum observed by mutating operations.
  size_t maxProbeLength() const noexcept { return max_probe_length_; }

  const FailureCounters &failureCounters() const noexcept { return failures_; }

private:
  static constexpr uint64_t kEmptyKey = 0;

  struct Entry {
    uint64_t key{0};
    uint64_t packed_handle{0};
  };

  static_assert(sizeof(Entry) == 16,
                "OrderRefDirectory entries must remain cache compact");

  struct ProbeResult {
    size_t index{0};
    size_t length{0};
    bool found{false};
  };

  static size_t validateSlotCapacity(size_t slot_capacity) {
    if (slot_capacity < 2 || !std::has_single_bit(slot_capacity)) {
      throw std::invalid_argument(
          "OrderRefDirectory slot capacity must be a power of two >= 2");
    }
    return slot_capacity;
  }

  // SplitMix64's finalizer gives stable, high-quality avalanche across sparse
  // and sequential 64-bit ITCH order references.
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

    // The <= 0.5 load invariant guarantees an empty slot. Keep a defensive
    // return to make the hot operation total even if memory is corrupted.
    return {index, capacity(), false};
  }

  static constexpr uint64_t pack(Handle handle) noexcept {
    return (static_cast<uint64_t>(handle.stock_locate) << 32) |
           static_cast<uint64_t>(handle.pool_index);
  }

  static constexpr Handle unpack(uint64_t packed_handle) noexcept {
    return {static_cast<uint16_t>(packed_handle >> 32),
            static_cast<uint32_t>(packed_handle)};
  }

  size_t probeDistance(size_t home, size_t slot) const noexcept {
    return (slot - home) & (capacity() - 1);
  }

  void eraseAt(size_t hole) noexcept {
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

  void observeProbeLength(size_t length) noexcept {
    if (length > max_probe_length_) {
      max_probe_length_ = length;
    }
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
  size_t max_probe_length_{0};
  FailureCounters failures_{};
};
