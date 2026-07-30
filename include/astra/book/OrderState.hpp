#pragma once

#include <cstdint>
#include <type_traits>

namespace astra::book {

inline constexpr std::uint8_t kOrderStateActive = 1u << 0;
// Nasdaq order reference numbers are day-unique. Keep this bit after an order
// becomes dead so a malformed later A/F/U cannot reuse the same reference.
inline constexpr std::uint8_t kOrderStateSeen = 1u << 1;

struct alignas(16) OrderState {
  std::uint32_t qty;
  std::uint32_t price_page_handle;
  std::uint16_t price_level_index;
  std::uint16_t locate;
  std::uint8_t side;
  std::uint8_t flags;
  // Expected logical owner of price_page_handle. This closes the otherwise
  // undetectable same-stock/wrong-page handle-substitution case without a
  // price-root lookup on an existing-order mutation.
  std::uint16_t price_page_index;
};

constexpr bool isActive(const OrderState &state) noexcept {
  return (state.flags & kOrderStateActive) != 0;
}

constexpr bool wasSeen(const OrderState &state) noexcept {
  return (state.flags & kOrderStateSeen) != 0;
}

static_assert(sizeof(OrderState) == 16);
static_assert(alignof(OrderState) == 16);
static_assert(64 % sizeof(OrderState) == 0);
static_assert(std::is_trivial_v<OrderState>);
static_assert(std::is_trivially_copyable_v<OrderState>);

} // namespace astra::book
