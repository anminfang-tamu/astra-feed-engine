#pragma once

#include "astra/symbol/SymbolTier.hpp"

#include <cstddef>
#include <string_view>

namespace astra::book_capacity {

inline constexpr std::size_t kDefaultOrderCapacity = 64 * 1024;
inline constexpr std::size_t kActiveOrderCapacity = 256 * 1024;
inline constexpr std::size_t kHotOrderCapacity = 1024 * 1024;
inline constexpr std::size_t kUltraHotOrderCapacity = 4 * 1024 * 1024;

constexpr std::size_t orderCapacity(astra::symbol::SymbolTier tier) noexcept {
  switch (tier) {
  case astra::symbol::SymbolTier::UltraHot:
    return kUltraHotOrderCapacity;
  case astra::symbol::SymbolTier::Hot:
    return kHotOrderCapacity;
  case astra::symbol::SymbolTier::Active:
    return kActiveOrderCapacity;
  case astra::symbol::SymbolTier::Default:
  default:
    return kDefaultOrderCapacity;
  }
}

// These defaults preserve the original deployment profile. BookManager also
// exposes a per-locate override so production sizing can be supplied from
// measured daily high-water marks without changing OrderBook.
constexpr astra::symbol::SymbolTier
tierForTicker(std::string_view ticker) noexcept {
  if (ticker == "NVDA" || ticker == "TSLA" || ticker == "QQQ" ||
      ticker == "SPY" || ticker == "INTC") {
    return astra::symbol::SymbolTier::UltraHot;
  }
  if (ticker == "AAPL" || ticker == "MSFT" || ticker == "AMD" ||
      ticker == "AMZN" || ticker == "META" || ticker == "GOOGL" ||
      ticker == "GOOG") {
    return astra::symbol::SymbolTier::Hot;
  }
  if (ticker == "CSCO" || ticker == "ADBE" || ticker == "AVGO" ||
      ticker == "NFLX" || ticker == "PYPL" || ticker == "ORCL") {
    return astra::symbol::SymbolTier::Active;
  }
  return astra::symbol::SymbolTier::Default;
}

} // namespace astra::book_capacity
