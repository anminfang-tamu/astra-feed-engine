#pragma once

#include <cstdint>
#include <string_view>

namespace astra::book {

enum class MutationResult : std::uint8_t {
  Applied,
  InvalidLocate,
  BookNotPrepared,
  InvalidSide,
  InvalidQuantity,
  PriceOutOfRange,
  OrderNotFound,
  DuplicateOrderRef,
  LocateMismatch,
  QuantityExceeded,
  PricePageCapacityExceeded,
  OrderTableCapacityExceeded,
  QuantityOverflow,
  QuantityUnderflow,
  OrderCountOverflow,
  InvalidPricePage,
  MissingPriceLevel,
  StaleReservation,
  InvariantViolation,
  BookInvalid,
};

enum class LookupPath : std::uint8_t {
  None,
  Direct,
  FallbackPrimary,
  FallbackSecondary,
  Miss,
};

constexpr std::string_view mutationResultName(MutationResult result) noexcept {
  switch (result) {
  case MutationResult::Applied:
    return "applied";
  case MutationResult::InvalidLocate:
    return "invalid locate";
  case MutationResult::BookNotPrepared:
    return "book not prepared";
  case MutationResult::InvalidSide:
    return "invalid side";
  case MutationResult::InvalidQuantity:
    return "invalid quantity";
  case MutationResult::PriceOutOfRange:
    return "price out of range";
  case MutationResult::OrderNotFound:
    return "order not found";
  case MutationResult::DuplicateOrderRef:
    return "duplicate order reference";
  case MutationResult::LocateMismatch:
    return "stock locate mismatch";
  case MutationResult::QuantityExceeded:
    return "quantity exceeds remaining order";
  case MutationResult::PricePageCapacityExceeded:
    return "price page capacity exceeded";
  case MutationResult::OrderTableCapacityExceeded:
    return "order table capacity exceeded";
  case MutationResult::QuantityOverflow:
    return "quantity overflow";
  case MutationResult::QuantityUnderflow:
    return "quantity underflow";
  case MutationResult::OrderCountOverflow:
    return "order count overflow";
  case MutationResult::InvalidPricePage:
    return "invalid price page";
  case MutationResult::MissingPriceLevel:
    return "missing price level";
  case MutationResult::StaleReservation:
    return "stale reservation";
  case MutationResult::InvariantViolation:
    return "book invariant violation";
  case MutationResult::BookInvalid:
    return "book state is invalid";
  }
  return "unknown mutation result";
}

} // namespace astra::book
