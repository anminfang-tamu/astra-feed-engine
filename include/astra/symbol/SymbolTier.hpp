#pragma once

#include <cstdint>

namespace astra::symbol {

enum class SymbolTier : std::uint8_t {
  Default,
  Active,
  Hot,
  UltraHot,
};

} // namespace astra::symbol
