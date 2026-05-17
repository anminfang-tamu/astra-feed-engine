// OrderIdMap.hpp
#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

// Fixed-capacity open-addressed hash table:
//
//   uint64_t order_id  ->  uint32_t pool_index
//
// Design:
//   - Structure of Arrays:
//
//       keys_[slot]    = order id
//       indices_[slot] = pool index
//
//   - Linear probing
//   - Backward-shift deletion
//   - No growth
//   - No rehash
//   - No heap allocation after construction
//
// Important:
//   - kEmptyKey is reserved and cannot be inserted.
//   - kInvalidIdx is reserved and cannot be used as a pool index.
//   - Keep load factor around <= 50% for stable low-latency behavior.
class OrderIdMap {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();

  static constexpr uint64_t kEmptyKey = std::numeric_limits<uint64_t>::max();

  explicit OrderIdMap(uint32_t capacity_pow2)
      : capacity_(capacity_pow2), mask_(capacity_pow2 - 1),
        keys_(new uint64_t[capacity_pow2]),
        indices_(new uint32_t[capacity_pow2]) {
    assert(capacity_pow2 >= 2);
    assert((capacity_pow2 & (capacity_pow2 - 1)) == 0 &&
           "capacity must be power of two");

    clear();
  }

  OrderIdMap(const OrderIdMap &) = delete;
  OrderIdMap &operator=(const OrderIdMap &) = delete;

  OrderIdMap(OrderIdMap &&) noexcept = default;
  OrderIdMap &operator=(OrderIdMap &&) noexcept = default;

  uint32_t find(uint64_t order_id) const noexcept {
    assert(order_id != kEmptyKey);

    uint64_t h = hash(order_id) & mask_;

    while (true) {
      const uint64_t k = keys_[h];

      if (k == order_id) {
        return indices_[h];
      }

      if (k == kEmptyKey) {
        return kInvalidIdx;
      }

      h = (h + 1) & mask_;
    }
  }

  bool contains(uint64_t order_id) const noexcept {
    return find(order_id) != kInvalidIdx;
  }

  // Returns:
  //   true  -> inserted new key
  //   false -> updated existing key
  //
  // Precondition:
  //   table must not be full.
  bool insert(uint64_t order_id, uint32_t pool_index) noexcept {
    const InsertResult result = tryInsert(order_id, pool_index);
    assert(result != InsertResult::Full &&
           "OrderIdMap full; increase capacity");
    return result == InsertResult::Inserted;
  }

  enum class InsertResult : uint8_t { Inserted, Updated, Full };

  // Production-safe version.
  // Use this in places where you do not want release builds to silently loop
  // forever if the map is full.
  InsertResult tryInsert(uint64_t order_id, uint32_t pool_index) noexcept {
    assert(order_id != kEmptyKey);
    assert(pool_index != kInvalidIdx);

    if (size_ >= capacity_) {
      return InsertResult::Full;
    }

    uint64_t h = hash(order_id) & mask_;

    while (true) {
      const uint64_t k = keys_[h];

      if (k == kEmptyKey) {
        keys_[h] = order_id;
        indices_[h] = pool_index;
        ++size_;
        return InsertResult::Inserted;
      }

      if (k == order_id) {
        indices_[h] = pool_index;
        return InsertResult::Updated;
      }

      h = (h + 1) & mask_;
    }
  }

  bool erase(uint64_t order_id) noexcept {
    assert(order_id != kEmptyKey);

    uint64_t h = hash(order_id) & mask_;

    while (true) {
      const uint64_t k = keys_[h];

      if (k == kEmptyKey) {
        return false;
      }

      if (k == order_id) {
        break;
      }

      h = (h + 1) & mask_;
    }

    eraseAt(h);
    --size_;
    return true;
  }

  void clear() noexcept {
    std::memset(keys_.get(), 0xFF, capacity_ * sizeof(uint64_t));
    std::memset(indices_.get(), 0xFF, capacity_ * sizeof(uint32_t));
    size_ = 0;
  }

  uint32_t size() const noexcept { return size_; }

  uint32_t capacity() const noexcept { return capacity_; }

  bool empty() const noexcept { return size_ == 0; }

  double loadFactor() const noexcept {
    return static_cast<double>(size_) / static_cast<double>(capacity_);
  }

private:
  // MurmurHash3 64-bit finalizer.
  //
  // Slightly more expensive than one Fibonacci multiply, but safer for
  // structured/sequential exchange order IDs.
  static uint64_t hash(uint64_t k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
  }

  void eraseAt(uint64_t erase_pos) noexcept {
    uint64_t gap = erase_pos;
    uint64_t next = (gap + 1) & mask_;

    while (true) {
      const uint64_t next_key = keys_[next];

      if (next_key == kEmptyKey) {
        break;
      }

      const uint64_t natural = hash(next_key) & mask_;

      // We can move next -> gap if the key at `next` would have probed
      // through `gap` during lookup.
      //
      // Distance is calculated modulo capacity because the table is circular.
      const uint64_t dist_next_from_natural = (next - natural) & mask_;
      const uint64_t dist_gap_from_natural = (gap - natural) & mask_;

      if (dist_gap_from_natural <= dist_next_from_natural) {
        keys_[gap] = keys_[next];
        indices_[gap] = indices_[next];
        gap = next;
      }

      next = (next + 1) & mask_;
    }

    keys_[gap] = kEmptyKey;
    indices_[gap] = kInvalidIdx;
  }

private:
  uint32_t capacity_{0};
  uint64_t mask_{0};
  uint32_t size_{0};

  std::unique_ptr<uint64_t[]> keys_;
  std::unique_ptr<uint32_t[]> indices_;
};