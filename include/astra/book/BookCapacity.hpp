#pragma once

#include "astra/symbol/SymbolTier.hpp"

#include <cstddef>
#include <string_view>

namespace astra::book_capacity {

inline constexpr std::size_t kDefaultOrderCapacity = 64 * 1024; // 64k
inline constexpr std::size_t kActiveOrderCapacity = 256 * 1024; // 256k
inline constexpr std::size_t kHotOrderCapacity = 1024 * 1024;   // 1M
inline constexpr std::size_t kUltraHotOrderCapacity =
    4 * 1024 * 1024; // 4M

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

constexpr bool tickerEquals(std::string_view ticker,
                            std::string_view configured) noexcept {
  if (ticker.size() != configured.size())
    return false;
  for (std::size_t i = 0; i < ticker.size(); ++i) {
    if (ticker[i] != configured[i])
      return false;
  }
  return true;
}

constexpr astra::symbol::SymbolTier
tierForTicker(std::string_view ticker) noexcept {
  if (tickerEquals(ticker, "NVDA") || tickerEquals(ticker, "TSLA") ||
      tickerEquals(ticker, "QQQ") || tickerEquals(ticker, "SPY") ||
      tickerEquals(ticker, "INTC")) {
    return astra::symbol::SymbolTier::UltraHot;
  }

  if (tickerEquals(ticker, "AAPL") || tickerEquals(ticker, "MSFT") ||
      tickerEquals(ticker, "AMD") || tickerEquals(ticker, "AMZN") ||
      tickerEquals(ticker, "META") || tickerEquals(ticker, "GOOGL") ||
      tickerEquals(ticker, "GOOG")) {
    return astra::symbol::SymbolTier::Hot;
  }

  if (tickerEquals(ticker, "CSCO") || tickerEquals(ticker, "ADBE") ||
      tickerEquals(ticker, "AVGO") || tickerEquals(ticker, "NFLX") ||
      tickerEquals(ticker, "PYPL") || tickerEquals(ticker, "ORCL")) {
    return astra::symbol::SymbolTier::Active;
  }

  return astra::symbol::SymbolTier::Default;
}

} // namespace astra::book_capacity
