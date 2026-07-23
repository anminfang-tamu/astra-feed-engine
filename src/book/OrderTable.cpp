#include "astra/book/OrderTable.hpp"

#include <limits>
#include <stdexcept>

namespace astra::book {

OrderTableConfig OrderTable::validateConfig(OrderTableConfig config) {
  if (config.direct_slots == 0 ||
      config.direct_slots > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(
        "OrderTable direct_slots must fit nonzero size_t");
  }
  if (config.fallback_bucket_count == 0 ||
      (config.fallback_bucket_count &
       (config.fallback_bucket_count - 1)) != 0) {
    throw std::invalid_argument(
        "OrderTable fallback_bucket_count must be a power of two");
  }
  return config;
}

OrderTable::OrderTable(OrderTableConfig config)
    : config_(validateConfig(config)),
      direct_(static_cast<std::size_t>(config_.direct_slots),
              kDirectStorageAlignment, "astra-order-direct"),
      fallback_(config_.fallback_bucket_count,
                kFallbackStorageAlignment, "astra-order-fallback") {
  if (config_.prefault) {
    direct_.prefault();
    fallback_.prefault();
  }
}

std::uint64_t OrderTable::hashPrimary(std::uint64_t value) noexcept {
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value;
}

std::uint64_t OrderTable::hashSecondary(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

std::array<std::uint32_t, OrderTable::kFallbackCandidateBuckets>
OrderTable::fallbackCandidateBuckets(std::uint64_t order_ref) const noexcept {
  const std::uint32_t mask = config_.fallback_bucket_count - 1;
  const std::uint32_t primary =
      static_cast<std::uint32_t>(hashPrimary(order_ref)) & mask;
  std::uint32_t secondary =
      static_cast<std::uint32_t>(hashSecondary(order_ref)) & mask;
  if (secondary == primary && config_.fallback_bucket_count > 1)
    secondary = (secondary + 1) & mask;
  return {primary, secondary};
}

OrderTable::FallbackEntry *
OrderTable::fallbackEntry(std::uint32_t bucket, std::uint8_t way) noexcept {
  return &fallback_[bucket].entries[way];
}

const OrderTable::FallbackEntry *OrderTable::fallbackEntry(
    std::uint32_t bucket, std::uint8_t way) const noexcept {
  return &fallback_[bucket].entries[way];
}

OrderState *OrderTable::findFallbackState(std::uint64_t order_ref) noexcept {
  NoopFallbackProbeObserver observer;
  return findFallbackStateObserved(order_ref, observer);
}

const OrderState *
OrderTable::findFallbackState(std::uint64_t order_ref) const noexcept {
  NoopFallbackProbeObserver observer;
  return findFallbackStateObserved(order_ref, observer);
}

OrderLookup OrderTable::find(std::uint64_t order_ref) noexcept {
  if (order_ref < config_.direct_slots) {
    OrderState &state = direct_[static_cast<std::size_t>(order_ref)];
    return isActive(state)
               ? OrderLookup{MutationResult::Applied, LookupPath::Direct,
                             &state}
               : OrderLookup{MutationResult::OrderNotFound,
                             LookupPath::Direct, nullptr};
  }

  const auto buckets = fallbackCandidateBuckets(order_ref);
  const std::uint32_t candidate_count =
      buckets[0] == buckets[1] ? 1u : kFallbackCandidateBuckets;
  for (std::uint32_t candidate = 0; candidate < candidate_count;
       ++candidate) {
    for (std::uint8_t way = 0; way < kFallbackWays; ++way) {
      FallbackEntry *entry = fallbackEntry(buckets[candidate], way);
      if (isActive(entry->state) && entry->order_ref == order_ref) {
        return {MutationResult::Applied,
                candidate == 0 ? LookupPath::FallbackPrimary
                               : LookupPath::FallbackSecondary,
                &entry->state};
      }
    }
  }
  return {MutationResult::OrderNotFound, LookupPath::Miss, nullptr};
}

ConstOrderLookup OrderTable::find(std::uint64_t order_ref) const noexcept {
  if (order_ref < config_.direct_slots) {
    const OrderState &state = direct_[static_cast<std::size_t>(order_ref)];
    return isActive(state)
               ? ConstOrderLookup{MutationResult::Applied,
                                  LookupPath::Direct, &state}
               : ConstOrderLookup{MutationResult::OrderNotFound,
                                  LookupPath::Direct, nullptr};
  }

  const auto buckets = fallbackCandidateBuckets(order_ref);
  const std::uint32_t candidate_count =
      buckets[0] == buckets[1] ? 1u : kFallbackCandidateBuckets;
  for (std::uint32_t candidate = 0; candidate < candidate_count;
       ++candidate) {
    for (std::uint8_t way = 0; way < kFallbackWays; ++way) {
      const FallbackEntry *entry = fallbackEntry(buckets[candidate], way);
      if (isActive(entry->state) && entry->order_ref == order_ref) {
        return {MutationResult::Applied,
                candidate == 0 ? LookupPath::FallbackPrimary
                               : LookupPath::FallbackSecondary,
                &entry->state};
      }
    }
  }
  return {MutationResult::OrderNotFound, LookupPath::Miss, nullptr};
}

OrderSlotReservation
OrderTable::reserve(std::uint64_t order_ref) const noexcept {
  if (order_ref < config_.direct_slots) {
    const OrderState &state = direct_[static_cast<std::size_t>(order_ref)];
    return isActive(state)
               ? OrderSlotReservation{MutationResult::DuplicateOrderRef,
                                      LookupPath::Direct, order_ref, 0, 0}
               : OrderSlotReservation{MutationResult::Applied,
                                      LookupPath::Direct, order_ref, 0, 0};
  }

  const auto buckets = fallbackCandidateBuckets(order_ref);
  const std::uint32_t candidate_count =
      buckets[0] == buckets[1] ? 1u : kFallbackCandidateBuckets;
  OrderSlotReservation first_empty{
      MutationResult::OrderTableCapacityExceeded, LookupPath::None,
      order_ref, 0, 0};

  // Scan all candidates for a duplicate before returning an empty slot.
  for (std::uint32_t candidate = 0; candidate < candidate_count;
       ++candidate) {
    for (std::uint8_t way = 0; way < kFallbackWays; ++way) {
      const FallbackEntry *entry = fallbackEntry(buckets[candidate], way);
      if (isActive(entry->state)) {
        if (entry->order_ref == order_ref) {
          return {MutationResult::DuplicateOrderRef,
                  candidate == 0 ? LookupPath::FallbackPrimary
                                 : LookupPath::FallbackSecondary,
                  order_ref, buckets[candidate], way};
        }
      } else if (!first_empty.valid()) {
        first_empty = {
            MutationResult::Applied,
            candidate == 0 ? LookupPath::FallbackPrimary
                           : LookupPath::FallbackSecondary,
            order_ref, buckets[candidate], way};
      }
    }
  }
  return first_empty;
}

MutationResult OrderTable::commit(const OrderSlotReservation &reservation,
                                  OrderState state) noexcept {
  if (!reservation.valid())
    return reservation.result;

  state.flags = static_cast<std::uint8_t>(state.flags | kOrderStateActive);
  if (reservation.path == LookupPath::Direct) {
    if (reservation.order_ref >= config_.direct_slots)
      return MutationResult::StaleReservation;
    OrderState &destination =
        direct_[static_cast<std::size_t>(reservation.order_ref)];
    if (isActive(destination))
      return MutationResult::StaleReservation;
    destination = state;
    return MutationResult::Applied;
  }

  if ((reservation.path != LookupPath::FallbackPrimary &&
       reservation.path != LookupPath::FallbackSecondary) ||
      reservation.bucket >= config_.fallback_bucket_count ||
      reservation.way >= kFallbackWays ||
      reservation.order_ref < config_.direct_slots) {
    return MutationResult::StaleReservation;
  }

  const auto candidate_buckets =
      fallbackCandidateBuckets(reservation.order_ref);
  const bool primary = reservation.path == LookupPath::FallbackPrimary;
  if (reservation.bucket != candidate_buckets[primary ? 0u : 1u] ||
      (!primary && candidate_buckets[0] == candidate_buckets[1])) {
    return MutationResult::StaleReservation;
  }

  FallbackEntry &destination =
      *fallbackEntry(reservation.bucket, reservation.way);
  if (isActive(destination.state))
    return MutationResult::StaleReservation;
  destination.order_ref = reservation.order_ref;
  destination.state = state;
  return MutationResult::Applied;
}

MutationResult OrderTable::erase(OrderState *state) noexcept {
  if (state == nullptr)
    return MutationResult::OrderNotFound;
  *state = OrderState{};
  return MutationResult::Applied;
}

MutationResult OrderTable::erase(const OrderLookup &lookup) noexcept {
  return lookup.found() ? erase(lookup.state) : lookup.result;
}

MutationResult OrderTable::erase(std::uint64_t order_ref) noexcept {
  return erase(findState(order_ref));
}

} // namespace astra::book
