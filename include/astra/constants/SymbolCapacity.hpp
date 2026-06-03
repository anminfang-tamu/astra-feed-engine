#pragma once

#include <cstddef>
#include <string_view>

namespace astra::capacity {

enum class SymbolTier : unsigned char { Default, Active, Hot, UltraHot };

static constexpr std::size_t kDefaultLiveOrderCapacity = 128 * 1024;   // 128k
static constexpr std::size_t kActiveLiveOrderCapacity = 512 * 1024;    // 512k
static constexpr std::size_t kHotLiveOrderCapacity = 1024 * 1024;      // 1M
static constexpr std::size_t kUltraHotLiveOrderCapacity = 2048 * 1024; // 2M

constexpr std::size_t orderCapacity(SymbolTier tier) noexcept {
  switch (tier) {
  case SymbolTier::UltraHot:
    return kUltraHotLiveOrderCapacity;
  case SymbolTier::Hot:
    return kHotLiveOrderCapacity;
  case SymbolTier::Active:
    return kActiveLiveOrderCapacity;
  case SymbolTier::Default:
  default:
    return kDefaultLiveOrderCapacity;
  }
}

constexpr SymbolTier tierForTicker(std::string_view ticker) noexcept {
  if (ticker == "NVDA" || ticker == "TSLA" || ticker == "QQQ" ||
      ticker == "SPY") {
    return SymbolTier::Hot;
  }

  if (ticker == "AAPL" || ticker == "MSFT" || ticker == "AMD" ||
      ticker == "AMZN" || ticker == "META" || ticker == "GOOGL" ||
      ticker == "GOOG") {
    return SymbolTier::Active;
  }

  return SymbolTier::Default;
}

} // namespace astra::capacity
