#pragma once

#include <cstdint>

namespace astra::symbol {

struct StockDirectoryEntry {
  std::uint32_t round_lot_size{
      0}; // e.g. 100 for each trade must be a multiple of this
  std::uint16_t locate{0};
  char ticker[9]{};       // 8 ASCII + null terminator, space-trimmed
  char market_category{}; // 'Q','R','S' (Nasdaq tiers), 'N','A','P' ...
  char fin_status{};      // Financial Status Indicator: 'D','E','Q'...
  bool is_test{false};    // Authenticity == 'T'
  bool round_lots_only{
      false}; // 'Y' if only round lots allowed (e.g. 100 shares with round lot
              // size 100), 'N' if odd lots allowed (e.g. 150 shares with round
              // lot size 100)
  char issue_classification{}; // 'A' = American Depositary Receipt (ADR), 'G' =
                               // Common Stock, 'M' = REIT, 'P' = Preferred
                               // Stock, 'S' = Structured Product, 'W' =
                               // Warrant, 'Z' = Other
};

} // namespace astra::symbol
