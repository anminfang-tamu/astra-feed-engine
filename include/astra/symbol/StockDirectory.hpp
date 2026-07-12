#pragma once

#include "astra/symbol/StockDirectoryEntry.hpp"
#include "astra/symbol/StockLocate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace astra::symbol {

// Fixed-size Stock Directory indexed directly by ITCH Stock Locate. O(1)
// access with no dynamic allocation; stock locate 0 is
// reserved/invalid.
class StockDirectory {
public:
  static constexpr std::uint16_t kMaxLocate = kMaxStockLocate;

  // Populate from a parsed Stock Directory ('R') message.
  void set(std::uint16_t locate, const StockDirectoryEntry &entry) noexcept {
    if (!isValidStockLocate(locate))
      return;
    if (entries_[locate].ticker[0] == '\0')
      ++count_;
    entries_[locate] = entry;
    entries_[locate].locate = locate;
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

  std::size_t size() const noexcept { return count_; }

private:
  // Zero-initialized; ticker[0] == '\0' indicates an unregistered locate slot.
  std::array<StockDirectoryEntry, kMaxLocate> entries_{};
  std::size_t count_{0};
};

} // namespace astra::symbol
