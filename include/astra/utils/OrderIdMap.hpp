// OrderIdMap.hpp
#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

// Fixed-capacity lazy mmap open-addressed hash table:
//
//   uint64_t order_id -> uint32_t pool_index
//
// Design:
//   - SoA layout:
//       keys_[slot]    = order id
//       indices_[slot] = pool index
//
//   - Linear probing
//   - Backward-shift deletion
//   - mmap allocation
//   - No memset on construction
//   - No heap traffic in hot path
//   - No rehash
//   - No growth
//
// Important:
//   - kEmptyKey is 0.
//   - order_id == 0 is reserved and cannot be inserted.
//   - indices_ is not initialized because it is only read after key match.
//   - Anonymous mmap memory is zero-filled lazily by Linux.
class OrderIdMap {
public:
  static constexpr uint64_t kEmptyKey = 0;

  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();

  explicit OrderIdMap(uint32_t capacity_pow2)
      : capacity_(capacity_pow2),
        mask_(static_cast<uint64_t>(capacity_pow2 - 1)) {
    assert(capacity_pow2 >= 2);
    assert((capacity_pow2 & (capacity_pow2 - 1)) == 0 &&
           "capacity must be power of two");

    keys_bytes_ =
        roundUpToPageSize(static_cast<size_t>(capacity_) * sizeof(uint64_t));

    indices_bytes_ =
        roundUpToPageSize(static_cast<size_t>(capacity_) * sizeof(uint32_t));

    keys_ = static_cast<uint64_t *>(::mmap(nullptr, keys_bytes_,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

    if (keys_ == MAP_FAILED) {
      keys_ = nullptr;
      std::abort();
    }

    indices_ = static_cast<uint32_t *>(
        ::mmap(nullptr, indices_bytes_, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

    if (indices_ == MAP_FAILED) {
      ::munmap(keys_, keys_bytes_);
      keys_ = nullptr;
      indices_ = nullptr;
      std::abort();
    }

#ifdef MADV_HUGEPAGE
    // Optional. Linux may ignore this.
    // Good for large maps because it can reduce TLB misses.
    ::madvise(keys_, keys_bytes_, MADV_HUGEPAGE);
    ::madvise(indices_, indices_bytes_, MADV_HUGEPAGE);
#endif
  }

  ~OrderIdMap() {
    if (keys_) {
      ::munmap(keys_, keys_bytes_);
    }

    if (indices_) {
      ::munmap(indices_, indices_bytes_);
    }
  }

  OrderIdMap(const OrderIdMap &) = delete;
  OrderIdMap &operator=(const OrderIdMap &) = delete;

  OrderIdMap(OrderIdMap &&other) noexcept { moveFrom(other); }

  OrderIdMap &operator=(OrderIdMap &&other) noexcept {
    if (this != &other) {
      this->~OrderIdMap();
      moveFrom(other);
    }

    return *this;
  }

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

  enum class InsertResult : uint8_t { Inserted, Updated, Full };

  bool insert(uint64_t order_id, uint32_t pool_index) noexcept {
    const InsertResult result = tryInsert(order_id, pool_index);

    assert(result != InsertResult::Full &&
           "OrderIdMap full; increase capacity");

    return result == InsertResult::Inserted;
  }

  InsertResult tryInsert(uint64_t order_id, uint32_t pool_index) noexcept {
    assert(order_id != kEmptyKey);
    assert(pool_index != kInvalidIdx);

    if (size_ >= capacity_) {
      return InsertResult::Full;
    }

    uint64_t h = hash(order_id) & mask_;

    while (true) {
      const uint64_t k = keys_[h];

      if (k == order_id) {
        indices_[h] = pool_index;
        return InsertResult::Updated;
      }

      if (k == kEmptyKey) {
        keys_[h] = order_id;
        indices_[h] = pool_index;
        ++size_;
        return InsertResult::Inserted;
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

  // This is lazy-friendly because MADV_DONTNEED releases physical pages
  // and returns them to zero-fill-on-demand state.
  void clear() noexcept {
    if (keys_) {
      ::madvise(keys_, keys_bytes_, MADV_DONTNEED);
    }

    if (indices_) {
      ::madvise(indices_, indices_bytes_, MADV_DONTNEED);
    }

    size_ = 0;
  }

  uint32_t size() const noexcept { return size_; }

  uint32_t capacity() const noexcept { return capacity_; }

  bool empty() const noexcept { return size_ == 0; }

  double loadFactor() const noexcept {
    return static_cast<double>(size_) / static_cast<double>(capacity_);
  }

private:
  static size_t pageSize() noexcept {
    static const size_t page_size =
        static_cast<size_t>(::sysconf(_SC_PAGESIZE));

    return page_size;
  }

  static size_t roundUpToPageSize(size_t n) noexcept {
    const size_t page = pageSize();
    return (n + page - 1) & ~(page - 1);
  }

  // MurmurHash3 64-bit finalizer.
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

  void moveFrom(OrderIdMap &other) noexcept {
    capacity_ = other.capacity_;
    mask_ = other.mask_;
    size_ = other.size_;

    keys_ = other.keys_;
    indices_ = other.indices_;

    keys_bytes_ = other.keys_bytes_;
    indices_bytes_ = other.indices_bytes_;

    other.capacity_ = 0;
    other.mask_ = 0;
    other.size_ = 0;

    other.keys_ = nullptr;
    other.indices_ = nullptr;

    other.keys_bytes_ = 0;
    other.indices_bytes_ = 0;
  }

private:
  uint32_t capacity_{0};
  uint64_t mask_{0};
  uint32_t size_{0};

  uint64_t *keys_{nullptr};
  uint32_t *indices_{nullptr};

  size_t keys_bytes_{0};
  size_t indices_bytes_{0};
};