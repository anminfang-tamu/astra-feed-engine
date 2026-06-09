#pragma once

#include <cstdint>

namespace astra::symbol {

inline constexpr std::uint16_t kInvalidStockLocate = 0;
inline constexpr std::uint16_t kMaxStockLocate = 16384;

constexpr bool isValidStockLocate(std::uint16_t locate) noexcept {
  return locate != kInvalidStockLocate && locate < kMaxStockLocate;
}

} // namespace astra::symbol
