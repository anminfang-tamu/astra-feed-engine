// OrderIdMap.hpp
#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <new>

class OrderIdMap {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();
  static constexpr uint64_t kEmptyKey = std::numeric_limits<uint64_t>::max();

  // capacity_pow2 must be a power of two; load factor target ~0.5.
  // Buckets are allocated lazily on first insert so empty books stay cheap.
  explicit OrderIdMap(uint32_t capacity_pow2) noexcept
      : max_capacity_(capacity_pow2) {}

  // Returns kInvalidIdx if not found.
  uint32_t find(uint64_t order_id) const noexcept {
    if (!buckets_) {
      return kInvalidIdx;
    }

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
    (void)tryInsert(order_id, pool_index);
  }

  bool tryInsert(uint64_t order_id, uint32_t pool_index) noexcept {
    if (buckets_) {
      uint64_t h = hash(order_id) & mask_;
      for (uint32_t probe = 0; probe < capacity_; ++probe) {
        Bucket &b = buckets_[h];
        if (b.order_id == order_id) {
          b.pool_index = pool_index;
          return true;
        }
        if (b.order_id == kEmptyKey) {
          break;
        }
        h = (h + 1) & mask_;
      }
    }

    if (!ensureCapacityForInsert()) {
      return false;
    }

    uint64_t h = hash(order_id) & mask_;
    while (true) {
      Bucket &b = buckets_[h];
      if (b.order_id == kEmptyKey || b.order_id == order_id) {
        b.order_id = order_id;
        b.pool_index = pool_index;
        ++size_;
        return true;
      }
      h = (h + 1) & mask_;
    }
  }

  // Remove and reinsert the following probe cluster to preserve lookup invariants.
  bool erase(uint64_t order_id) noexcept {
    if (!buckets_) {
      return false;
    }

    uint64_t h = hash(order_id) & mask_;
    while (true) {
      Bucket &b = buckets_[h];
      if (b.order_id == kEmptyKey)
        return false;
      if (b.order_id == order_id) {
        clearBucket(b);
        --size_;

        uint64_t next = (h + 1) & mask_;
        while (buckets_[next].order_id != kEmptyKey) {
          const Bucket moved = buckets_[next];
          clearBucket(buckets_[next]);
          --size_;
          insertInto(buckets_.get(), mask_, moved.order_id, moved.pool_index);
          ++size_;
          next = (next + 1) & mask_;
        }

        return true;
      }
      h = (h + 1) & mask_;
    }
  }

private:
  static constexpr uint32_t kInitialCapacity = 1024;

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

  bool ensureCapacityForInsert() noexcept {
    if (max_capacity_ == 0) {
      return false;
    }

    if (capacity_ == 0) {
      const uint32_t initial_capacity =
          max_capacity_ < kInitialCapacity ? max_capacity_ : kInitialCapacity;
      return rehash(initial_capacity);
    }

    if ((static_cast<uint64_t>(size_) + 1) * 2 <= capacity_) {
      return true;
    }

    if (capacity_ >= max_capacity_) {
      return size_ < capacity_;
    }

    uint32_t next_capacity = capacity_ << 1;
    if (next_capacity <= capacity_ || next_capacity > max_capacity_) {
      next_capacity = max_capacity_;
    }

    if (next_capacity == capacity_) {
      return true;
    }

    return rehash(next_capacity) || size_ < capacity_;
  }

  bool rehash(uint32_t capacity) noexcept {
    std::unique_ptr<Bucket[]> next(new (std::nothrow) Bucket[capacity]);
    if (!next) {
      return false;
    }

    const uint64_t next_mask = static_cast<uint64_t>(capacity - 1);
    for (uint32_t i = 0; i < capacity_; ++i) {
      const Bucket &bucket = buckets_[i];
      if (bucket.order_id != kEmptyKey) {
        insertInto(next.get(), next_mask, bucket.order_id, bucket.pool_index);
      }
    }

    buckets_ = std::move(next);
    capacity_ = capacity;
    mask_ = next_mask;
    return true;
  }

  static void insertInto(Bucket *buckets, uint64_t mask, uint64_t order_id,
                         uint32_t pool_index) noexcept {
    uint64_t h = hash(order_id) & mask;
    while (true) {
      Bucket &bucket = buckets[h];
      if (bucket.order_id == kEmptyKey) {
        bucket.order_id = order_id;
        bucket.pool_index = pool_index;
        return;
      }
      h = (h + 1) & mask;
    }
  }

  static void clearBucket(Bucket &bucket) noexcept {
    bucket.order_id = kEmptyKey;
    bucket.pool_index = kInvalidIdx;
  }

  uint64_t mask_{0};
  uint32_t max_capacity_{0};
  uint32_t capacity_{0};
  uint32_t size_{0};
  std::unique_ptr<Bucket[]> buckets_;
};
