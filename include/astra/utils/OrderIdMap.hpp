// OrderIdMap.hpp
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

class OrderIdMap {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();
  static constexpr uint64_t kEmptyKey = std::numeric_limits<uint64_t>::max();

  // capacity_pow2 must be a power of two; load factor target ~0.5
  explicit OrderIdMap(uint32_t capacity_pow2) noexcept
      : mask_(capacity_pow2 - 1),
        buckets_(std::make_unique<Bucket[]>(capacity_pow2)) {
    // Bucket default-initializes order_id=kEmptyKey via in-class initializer
  }

  // Returns kInvalidIdx if not found.
  uint32_t find(uint64_t order_id) const noexcept {
    uint64_t h = hash(order_id) & mask_;
    while (true) {
      const Bucket &b = buckets_[h];
      if (b.order_id == order_id)
        return b.pool_index;
      if (b.order_id == kEmptyKey)
        return kInvalidIdx;
      h = (h + 1) & mask_;
    }
  }

  // Insert or update. Caller guarantees room (load factor managed externally).
  void insert(uint64_t order_id, uint32_t pool_index) noexcept {
    uint64_t h = hash(order_id) & mask_;
    while (true) {
      Bucket &b = buckets_[h];
      if (b.order_id == kEmptyKey || b.order_id == order_id) {
        b.order_id = order_id;
        b.pool_index = pool_index;
        return;
      }
      h = (h + 1) & mask_;
    }
  }

  // Remove using backward-shift deletion (preserves probe sequence invariants).
  bool erase(uint64_t order_id) noexcept {
    uint64_t h = hash(order_id) & mask_;
    while (true) {
      Bucket &b = buckets_[h];
      if (b.order_id == kEmptyKey)
        return false;
      if (b.order_id == order_id) {
        // Backward-shift: walk forward, move any entry whose ideal slot is ≤ h
        uint64_t i = h;
        while (true) {
          uint64_t j = (i + 1) & mask_;
          Bucket &bj = buckets_[j];
          if (bj.order_id == kEmptyKey) {
            buckets_[i].order_id = kEmptyKey;
            return true;
          }
          const uint64_t ideal = hash(bj.order_id) & mask_;
          // bj belongs at 'ideal'; move it to i only if i is reachable from
          // ideal without crossing the original slot (the classic Robin Hood
          // backshift check)
          if (distance(i, ideal) < distance(j, ideal)) {
            buckets_[i] = bj;
            i = j;
          } else {
            buckets_[i].order_id = kEmptyKey;
            return true;
          }
        }
      }
      h = (h + 1) & mask_;
    }
  }

private:
  struct Bucket {
    uint64_t order_id = kEmptyKey; // sentinel for empty
    uint32_t pool_index = kInvalidIdx;
    uint32_t _pad = 0; // 16-byte bucket — pair fits cache line nicely
  };
  static_assert(sizeof(Bucket) == 16, "Bucket should be 16 bytes");

  // Fast integer hash — Murmur3 64-bit finalizer. Order IDs are not adversarial
  // here, but they may be sequential or clustered, so we still mix them.
  static uint64_t hash(uint64_t k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
  }

  uint64_t distance(uint64_t pos, uint64_t ideal) const noexcept {
    return (pos - ideal) & mask_;
  }

  uint64_t mask_;
  std::unique_ptr<Bucket[]> buckets_;
};