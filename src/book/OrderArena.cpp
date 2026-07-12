#include "astra/book/OrderArena.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace {

template <typename T> void touchPages(std::vector<T> &storage) noexcept {
  if (storage.empty()) {
    return;
  }

  // A 4 KiB stride touches every page even on systems with larger pages.
  constexpr size_t kTouchStride = 4096;
  const size_t byte_count = storage.size() * sizeof(T);
  volatile std::byte *bytes =
      reinterpret_cast<volatile std::byte *>(storage.data());
  for (size_t offset = 0; offset < byte_count; offset += kTouchStride) {
    bytes[offset] = bytes[offset];
  }
  bytes[byte_count - 1] = bytes[byte_count - 1];
}

} // namespace

size_t OrderArena::checkedCapacity(size_t capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("order arena capacity must be nonzero");
  }
  if (capacity > std::numeric_limits<Index>::max()) {
    throw std::length_error("order arena exceeds 32-bit index domain");
  }
  return capacity;
}

OrderArena::OrderArena(size_t capacity)
    : capacity_(checkedCapacity(capacity)), orders_(capacity_),
      free_indices_(capacity_),
      occupancy_(capacity_ / kOccupancyWordBits +
                     (capacity_ % kOccupancyWordBits != 0 ? 1 : 0),
                 uint64_t{0}),
      free_count_(capacity_) {
  // Pop from the back to allocate 0, 1, ... on first use. A released index is
  // pushed at the back and is therefore the next one reused.
  for (size_t i = 0; i < capacity_; ++i) {
    free_indices_[i] = static_cast<Index>(capacity_ - i - 1);
  }

  // Sized vectors are value-initialized. Explicit page walks ensure the full
  // fixed working set is resident before the decode hot path starts.
  touchPages(orders_);
  touchPages(free_indices_);
  touchPages(occupancy_);
}

OrderArena::Index OrderArena::allocate() noexcept {
  if (free_count_ == 0) {
    ++allocation_failures_;
    return kInvalidIndex;
  }

  const Index index = free_indices_[--free_count_];
  markInUse(index);
  ++in_use_;
  high_watermark_ = std::max(high_watermark_, in_use_);
  return index;
}

bool OrderArena::release(Index index) noexcept {
  if (static_cast<size_t>(index) >= capacity_) {
    ++invalid_releases_;
    return false;
  }
  if (!isInUse(index)) {
    ++double_releases_;
    return false;
  }

  orders_[index] = Order{};
  markFree(index);
  free_indices_[free_count_++] = index;
  --in_use_;
  return true;
}

Order *OrderArena::at(Index index) noexcept {
  return isInUse(index) ? &orders_[index] : nullptr;
}

const Order *OrderArena::at(Index index) const noexcept {
  return isInUse(index) ? &orders_[index] : nullptr;
}

bool OrderArena::isInUse(Index index) const noexcept {
  if (static_cast<size_t>(index) >= capacity_) {
    return false;
  }
  const size_t word = index / kOccupancyWordBits;
  const uint64_t bit = uint64_t{1} << (index % kOccupancyWordBits);
  return (occupancy_[word] & bit) != 0;
}

void OrderArena::markInUse(Index index) noexcept {
  const size_t word = index / kOccupancyWordBits;
  occupancy_[word] |= uint64_t{1} << (index % kOccupancyWordBits);
}

void OrderArena::markFree(Index index) noexcept {
  const size_t word = index / kOccupancyWordBits;
  occupancy_[word] &= ~(uint64_t{1} << (index % kOccupancyWordBits));
}

OrderArenaStats OrderArena::stats() const noexcept {
  return OrderArenaStats{
      .capacity = capacity_,
      .in_use = in_use_,
      .high_watermark = high_watermark_,
      .allocation_failures = allocation_failures_,
      .invalid_releases = invalid_releases_,
      .double_releases = double_releases_,
  };
}
