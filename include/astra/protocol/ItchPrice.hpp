#pragma once

#include <cstdint>

namespace astra::protocol {

// Nasdaq TotalView-ITCH Price(4) fields are unsigned fixed-point values with
// four implied decimal places. The published maximum is $200,000.0000.
inline constexpr std::uint32_t kItchPrice4Scale = 10'000u;
inline constexpr std::uint32_t kMaxItchPrice4 = 2'000'000'000u;

constexpr bool isValidItchPrice4(std::uint64_t raw_price) noexcept {
  return raw_price <= kMaxItchPrice4;
}

} // namespace astra::protocol
