#pragma once

#include <cstddef>
#include <string_view>

namespace astra::capacity {

enum class SymbolTier : unsigned char {
  Default,
  Active,
  Hot,
};

static constexpr std::size_t kDefaultOrderCapacity = 16 * 1024; // 16k
static constexpr std::size_t kActiveOrderCapacity = 100 * 1024; // 100k
static constexpr std::size_t kHotOrderCapacity = 512 * 1024;    // 512k

constexpr std::size_t orderCapacity(SymbolTier tier) noexcept {
  switch (tier) {
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
      tickerEquals(ticker, "QQQ") || tickerEquals(ticker, "SPY")) {
    return SymbolTier::Hot;
  }

  if (tickerEquals(ticker, "AAPL") || tickerEquals(ticker, "MSFT") ||
      tickerEquals(ticker, "AMD") || tickerEquals(ticker, "AMZN") ||
      tickerEquals(ticker, "META") || tickerEquals(ticker, "GOOGL") ||
      tickerEquals(ticker, "GOOG")) {
    return SymbolTier::Active;
  }

  return SymbolTier::Default;
}

} // namespace astra::capacity
