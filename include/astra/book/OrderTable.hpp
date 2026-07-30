#pragma once

#include "astra/book/MutationResult.hpp"
#include "astra/book/OrderState.hpp"
#include "astra/utils/MappedArray.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace astra::book {

struct OrderTableConfig {
  std::uint64_t direct_slots{1u << 20};
  std::uint32_t fallback_bucket_count{1u << 12};
  bool prefault{false};
};

struct OrderLookup {
  MutationResult result{MutationResult::OrderNotFound};
  LookupPath path{LookupPath::None};
  OrderState *state{nullptr};

  constexpr bool found() const noexcept {
    return result == MutationResult::Applied && state != nullptr;
  }
};

struct ConstOrderLookup {
  MutationResult result{MutationResult::OrderNotFound};
  LookupPath path{LookupPath::None};
  const OrderState *state{nullptr};

  constexpr bool found() const noexcept {
    return result == MutationResult::Applied && state != nullptr;
  }
};

struct OrderSlotReservation {
  MutationResult result{MutationResult::InvariantViolation};
  LookupPath path{LookupPath::None};
  std::uint64_t order_ref{0};
  std::uint32_t bucket{0};
  std::uint8_t way{0};

  constexpr bool valid() const noexcept {
    return result == MutationResult::Applied;
  }
};

class OrderTable final {
public:
  struct NoopFallbackProbeObserver {
    constexpr void onFallbackSlot() noexcept {}
  };

  // A fallback lookup has a compile-time, data-independent upper bound.
  static constexpr std::uint32_t kDirectSlotsExamined = 1;
  static constexpr std::uint32_t kFallbackCandidateBuckets = 2;
  static constexpr std::uint32_t kFallbackWays = 2;
  static constexpr std::uint32_t kMaxFallbackSlotsExamined =
      kFallbackCandidateBuckets * kFallbackWays;
  static constexpr std::size_t kHotArenaAlignment =
      2u * 1024u * 1024u;
  static constexpr std::size_t kDirectStorageAlignment =
      kHotArenaAlignment;
  static constexpr std::size_t kFallbackStorageAlignment =
      kHotArenaAlignment;
  // Public storage-layout constants let startup planning account for the
  // private fallback arena without exposing its implementation records.
  static constexpr std::size_t kFallbackBucketStorageBytes = 64;
  static constexpr std::size_t kFallbackBucketStorageAlignment = 64;

  explicit OrderTable(OrderTableConfig config);

  OrderTable(const OrderTable &) = delete;
  OrderTable &operator=(const OrderTable &) = delete;
  OrderTable(OrderTable &&) = delete;
  OrderTable &operator=(OrderTable &&) = delete;

  OrderLookup find(std::uint64_t order_ref) noexcept;
  ConstOrderLookup find(std::uint64_t order_ref) const noexcept;

  // Mutation hot path: callers only need the resident state pointer, not the
  // diagnostic LookupPath carried by find().  Keep the direct probe visible
  // to the caller while the larger bounded fallback scan remains out of line.
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((always_inline))
#endif
  OrderState *findState(std::uint64_t order_ref) noexcept {
    if (order_ref >= config_.direct_slots)
      return findFallbackState(order_ref);
    OrderState *const state =
        direct_.data() + static_cast<std::size_t>(order_ref);
    return isActive(*state) ? state : nullptr;
  }

#if defined(__GNUC__) || defined(__clang__)
  __attribute__((always_inline))
#endif
  const OrderState *findState(std::uint64_t order_ref) const noexcept {
    if (order_ref >= config_.direct_slots)
      return findFallbackState(order_ref);
    const OrderState *const state =
        direct_.data() + static_cast<std::size_t>(order_ref);
    return isActive(*state) ? state : nullptr;
  }

  // reserve() never changes table state. A reference remains reserved for the
  // entire session after erase because Nasdaq order references are day-unique.
  // Once all other mutation resources have been preflighted, commit()
  // publishes the record by setting active and seen.
  OrderSlotReservation reserve(std::uint64_t order_ref) const noexcept;
  MutationResult commit(const OrderSlotReservation &reservation,
                        OrderState state) noexcept;

  MutationResult erase(OrderState *state) noexcept;
  MutationResult erase(const OrderLookup &lookup) noexcept;
  MutationResult erase(std::uint64_t order_ref) noexcept;

  std::uint64_t directCapacity() const noexcept {
    return config_.direct_slots;
  }
  std::uint32_t fallbackBucketCount() const noexcept {
    return config_.fallback_bucket_count;
  }
  const OrderState *directData() const noexcept { return direct_.data(); }
  const void *directArenaBase() const noexcept { return direct_.data(); }
  std::size_t directArenaMappedBytes() const noexcept {
    return direct_.mappedBytes();
  }
  const void *fallbackArenaBase() const noexcept {
    return fallback_.data();
  }
  std::size_t fallbackArenaMappedBytes() const noexcept {
    return fallback_.mappedBytes();
  }

  std::array<std::uint32_t, kFallbackCandidateBuckets>
  fallbackCandidateBuckets(std::uint64_t order_ref) const noexcept;

  // The production fallback helper instantiates this exact loop with the
  // no-op observer. Tests can instantiate a counting observer to prove the
  // four-slot miss bound without adding a counter or branch to the hot path.
  template <typename WorkObserver>
  OrderState *findFallbackStateObserved(
      std::uint64_t order_ref, WorkObserver &observer) noexcept {
    const auto buckets = fallbackCandidateBuckets(order_ref);
    const std::uint32_t candidate_count =
        buckets[0] == buckets[1] ? 1u : kFallbackCandidateBuckets;
    for (std::uint32_t candidate = 0; candidate < candidate_count;
         ++candidate) {
      for (std::uint8_t way = 0; way < kFallbackWays; ++way) {
        observer.onFallbackSlot();
        FallbackEntry *const entry =
            fallbackEntry(buckets[candidate], way);
        if (isActive(entry->state) && entry->order_ref == order_ref)
          return &entry->state;
      }
    }
    return nullptr;
  }

  template <typename WorkObserver>
  const OrderState *findFallbackStateObserved(
      std::uint64_t order_ref, WorkObserver &observer) const noexcept {
    const auto buckets = fallbackCandidateBuckets(order_ref);
    const std::uint32_t candidate_count =
        buckets[0] == buckets[1] ? 1u : kFallbackCandidateBuckets;
    for (std::uint32_t candidate = 0; candidate < candidate_count;
         ++candidate) {
      for (std::uint8_t way = 0; way < kFallbackWays; ++way) {
        observer.onFallbackSlot();
        const FallbackEntry *const entry =
            fallbackEntry(buckets[candidate], way);
        if (isActive(entry->state) && entry->order_ref == order_ref)
          return &entry->state;
      }
    }
    return nullptr;
  }

private:
  struct alignas(32) FallbackEntry {
    OrderState state;
    std::uint64_t order_ref;
    std::uint64_t reserved;
  };

  struct alignas(64) FallbackBucket {
    std::array<FallbackEntry, kFallbackWays> entries;
  };

  static_assert(sizeof(FallbackEntry) == 32);
  static_assert(sizeof(FallbackBucket) == kFallbackBucketStorageBytes);
  static_assert(alignof(FallbackBucket) ==
                kFallbackBucketStorageAlignment);
  static_assert(std::is_trivial_v<FallbackEntry>);
  static_assert(std::is_trivial_v<FallbackBucket>);

  static OrderTableConfig validateConfig(OrderTableConfig config);
  static std::uint64_t hashPrimary(std::uint64_t value) noexcept;
  static std::uint64_t hashSecondary(std::uint64_t value) noexcept;

  OrderState *findFallbackState(std::uint64_t order_ref) noexcept;
  const OrderState *
  findFallbackState(std::uint64_t order_ref) const noexcept;

  FallbackEntry *fallbackEntry(std::uint32_t bucket,
                               std::uint8_t way) noexcept;
  const FallbackEntry *fallbackEntry(std::uint32_t bucket,
                                     std::uint8_t way) const noexcept;

  OrderTableConfig config_;
  astra::utils::MappedArray<OrderState> direct_;
  astra::utils::MappedArray<FallbackBucket> fallback_;
};

} // namespace astra::book
