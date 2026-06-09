
#pragma once

#include <cstdint>

namespace astra::symbol {

enum class SymbolTier : std::uint8_t {
  Unknown,
  Default,
  Active,
  Hot,
  UltraHot
};

} // namespace astra::symbol
