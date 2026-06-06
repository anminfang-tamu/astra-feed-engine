#pragma once

#include <cstddef>
#include <string_view>

namespace astra::capacity {

enum class SymbolTier : unsigned char { Default, Active, Hot, UltraHot };

static constexpr std::size_t kDefaultOrderCapacity = 64 * 1024;        // 64k
static constexpr std::size_t kActiveOrderCapacity = 256 * 1024;        // 256k
static constexpr std::size_t kHotOrderCapacity = 1024 * 1024;          // 1M
static constexpr std::size_t kUltraHotOrderCapacity = 4 * 1024 * 1024; // 4M

constexpr std::size_t orderCapacity(SymbolTier tier) noexcept {
  switch (tier) {
  case SymbolTier::UltraHot:
    return kUltraHotOrderCapacity;
  case SymbolTier::Hot:
    return kHotOrderCapacity;
  case SymbolTier::Active:
    return kActiveOrderCapacity;
  case SymbolTier::Default:
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

constexpr SymbolTier tierForTicker(std::string_view ticker) noexcept {
  if (tickerEquals(ticker, "NVDA") || tickerEquals(ticker, "TSLA") ||
      tickerEquals(ticker, "QQQ") || tickerEquals(ticker, "SPY") ||
      tickerEquals(ticker, "INTC")) {
    return SymbolTier::UltraHot;
  }

  if (tickerEquals(ticker, "AAPL") || tickerEquals(ticker, "MSFT") ||
      tickerEquals(ticker, "AMD") || tickerEquals(ticker, "AMZN") ||
      tickerEquals(ticker, "META") || tickerEquals(ticker, "GOOGL") ||
      tickerEquals(ticker, "GOOG")) {
    return SymbolTier::Hot;
  }

  if (tickerEquals(ticker, "CSCO") || tickerEquals(ticker, "ADBE") ||
      tickerEquals(ticker, "AVGO") || tickerEquals(ticker, "NFLX") ||
      tickerEquals(ticker, "PYPL") || tickerEquals(ticker, "ORCL")) {
    return SymbolTier::Active;
  }

  return SymbolTier::Default;
}

} // namespace astra::capacity
