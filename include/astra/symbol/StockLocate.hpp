#pragma once

#include <cstddef>
#include <cstdint>

namespace astra::symbol {

inline constexpr std::uint16_t kInvalidStockLocate = 0;
// Stock Locate is an unsigned 16-bit wire field.  Keep one direct-addressed
// slot for every representable value; locate zero remains reserved/invalid.
inline constexpr std::size_t kStockLocateSlots = 1u << 16;
inline constexpr std::size_t kMaxStockLocate = kStockLocateSlots;

constexpr bool isValidStockLocate(std::uint16_t locate) noexcept {
  return locate != kInvalidStockLocate;
}

} // namespace astra::symbol
