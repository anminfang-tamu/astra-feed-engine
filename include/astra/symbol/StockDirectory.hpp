#pragma once

#include "astra/symbol/StockDirectoryEntry.hpp"
#include "astra/symbol/StockLocate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace astra::symbol {

// Fixed-size Stock Directory indexed directly by ITCH Stock Locate. O(1)
// access with no dynamic allocation; stock locate 0 is
// reserved/invalid.
class StockDirectory {
public:
  static constexpr std::size_t kMaxLocate = kStockLocateSlots;

  // Populate from a parsed Stock Directory ('R') message.
  // A locate's ticker identity is immutable, and one ticker cannot identify
  // two locates. Metadata refreshes for the same (locate, ticker) pair remain
  // valid. The fixed reverse index keeps this administrative check
  // allocation-free without an O(number_of_symbols) scan for every R message.
  bool set(std::uint16_t locate,
           const StockDirectoryEntry &entry) noexcept {
    if (!isValidStockLocate(locate))
      return false;

    const std::string_view new_ticker = tickerView(entry);
    if (new_ticker.empty())
      return false;

    if (const StockDirectoryEntry *existing = get(locate);
        existing != nullptr) {
      if (tickerView(*existing) != new_ticker)
        return false;
      entries_[locate] = canonicalEntry(locate, entry, new_ticker);
      return true;
    }

    if (const std::uint16_t registered_locate =
            locateForTicker(new_ticker);
        registered_locate != kInvalidStockLocate) {
      return false;
    }

    const std::size_t reverse_slot = emptyReverseSlot(new_ticker);
    if (reverse_slot == kTickerIndexCapacity)
      return false;

    entries_[locate] = canonicalEntry(locate, entry, new_ticker);
    ticker_index_[reverse_slot] = locate;
    ++count_;
    return true;
  }

  const StockDirectoryEntry *get(std::uint16_t locate) const noexcept {
    if (!isValidStockLocate(locate) || entries_[locate].ticker[0] == '\0')
      return nullptr;
    return &entries_[locate];
  }

  const char *ticker(std::uint16_t locate) const noexcept {
    if (!isValidStockLocate(locate) || entries_[locate].ticker[0] == '\0')
      return nullptr;
    return entries_[locate].ticker;
  }

  bool isRegistered(std::uint16_t locate) const noexcept {
    return isValidStockLocate(locate) && entries_[locate].ticker[0] != '\0';
  }

  std::uint16_t locateForTicker(std::string_view ticker_value) const noexcept {
    if (ticker_value.empty())
      return kInvalidStockLocate;
    std::size_t slot = tickerHash(ticker_value) & kTickerIndexMask;
    for (std::size_t probe = 0; probe < kTickerIndexCapacity; ++probe) {
      const std::uint16_t locate = ticker_index_[slot];
      if (locate == kInvalidStockLocate)
        return kInvalidStockLocate;
      if (tickerView(entries_[locate]) == ticker_value)
        return locate;
      slot = (slot + 1) & kTickerIndexMask;
    }
    return kInvalidStockLocate;
  }

  std::size_t size() const noexcept { return count_; }

private:
  // At most 65,535 valid locates can be registered, so this power-of-two
  // table remains below 50% load and uses locate zero as its empty sentinel.
  static constexpr std::size_t kTickerIndexCapacity = 1u << 17;
  static constexpr std::size_t kTickerIndexMask =
      kTickerIndexCapacity - 1;

  static std::string_view
  tickerView(const StockDirectoryEntry &entry) noexcept {
    std::size_t length = 0;
    while (length < 8 && entry.ticker[length] != '\0')
      ++length;
    return {entry.ticker, length};
  }

  static StockDirectoryEntry
  canonicalEntry(std::uint16_t locate, const StockDirectoryEntry &entry,
                 std::string_view ticker_value) noexcept {
    StockDirectoryEntry canonical = entry;
    for (std::size_t index = ticker_value.size(); index < 9; ++index)
      canonical.ticker[index] = '\0';
    canonical.locate = locate;
    return canonical;
  }

  static std::size_t tickerHash(std::string_view ticker_value) noexcept {
    std::size_t hash =
        sizeof(std::size_t) == sizeof(std::uint64_t)
            ? static_cast<std::size_t>(14695981039346656037ULL)
            : static_cast<std::size_t>(2166136261U);
    const std::size_t prime =
        sizeof(std::size_t) == sizeof(std::uint64_t)
            ? static_cast<std::size_t>(1099511628211ULL)
            : static_cast<std::size_t>(16777619U);
    for (const unsigned char byte : ticker_value) {
      hash ^= byte;
      hash *= prime;
    }
    return hash;
  }

  std::size_t emptyReverseSlot(std::string_view ticker_value) const noexcept {
    std::size_t slot = tickerHash(ticker_value) & kTickerIndexMask;
    for (std::size_t probe = 0; probe < kTickerIndexCapacity; ++probe) {
      if (ticker_index_[slot] == kInvalidStockLocate)
        return slot;
      slot = (slot + 1) & kTickerIndexMask;
    }
    return kTickerIndexCapacity;
  }

  // Zero-initialized; ticker[0] == '\0' indicates an unregistered locate slot.
  std::array<StockDirectoryEntry, kMaxLocate> entries_{};
  std::array<std::uint16_t, kTickerIndexCapacity> ticker_index_{};
  std::size_t count_{0};
};

} // namespace astra::symbol
