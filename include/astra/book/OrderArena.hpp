#pragma once

#include "astra/book/Order.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct OrderArenaStats {
  size_t capacity{0};
  size_t in_use{0};
  size_t high_watermark{0};
  uint64_t allocation_failures{0};
  uint64_t invalid_releases{0};
  uint64_t double_releases{0};
};

// Fixed storage for Orders shared by all books. Construction allocates and
// touches every backing page; allocate(), release(), and at() never allocate.
// The arena is intentionally single-writer.
class OrderArena {
public:
  using Index = uint32_t;
  static constexpr Index kInvalidIndex =
      std::numeric_limits<Index>::max();

  explicit OrderArena(size_t capacity);

  OrderArena(const OrderArena &) = delete;
  OrderArena &operator=(const OrderArena &) = delete;
  OrderArena(OrderArena &&) = delete;
  OrderArena &operator=(OrderArena &&) = delete;

  // Returns kInvalidIndex when the fixed arena is exhausted.
  Index allocate() noexcept;

  // Returns false for an out-of-range index or an index that is already free.
  // A successful release resets the entire Order record before reuse.
  bool release(Index index) noexcept;

  // Only live indices resolve. The returned address remains stable for the
  // lifetime of the arena, including across all other allocate/release calls.
  Order *at(Index index) noexcept;
  const Order *at(Index index) const noexcept;

  bool isInUse(Index index) const noexcept;
  size_t capacity() const noexcept { return capacity_; }
  size_t inUse() const noexcept { return in_use_; }
  size_t highWatermark() const noexcept { return high_watermark_; }
  uint64_t allocationFailures() const noexcept {
    return allocation_failures_;
  }
  uint64_t invalidReleases() const noexcept { return invalid_releases_; }
  uint64_t doubleReleases() const noexcept { return double_releases_; }
  OrderArenaStats stats() const noexcept;

private:
  static size_t checkedCapacity(size_t capacity);
  static constexpr size_t kOccupancyWordBits = 64;

  void markInUse(Index index) noexcept;
  void markFree(Index index) noexcept;

  size_t capacity_;
  alignas(64) std::vector<Order> orders_;
  std::vector<Index> free_indices_;
  std::vector<uint64_t> occupancy_;
  size_t free_count_;
  size_t in_use_{0};
  size_t high_watermark_{0};
  uint64_t allocation_failures_{0};
  uint64_t invalid_releases_{0};
  uint64_t double_releases_{0};
};

static_assert(sizeof(Order) == 32,
              "OrderArena requires two Orders per 64-byte cache line");
