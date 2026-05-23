#pragma once
#include <cstdint>

struct SymbolInfo {
  uint32_t round_lot_size;
  uint16_t locate;
  char ticker[9];       // 8 ASCII + null terminator, space-trimmed
  char market_category; // 'Q','R','S' (Nasdaq tiers), 'N','A','P' ...
  char fin_status;      // Financial Status Indicator: 'D','E','Q'...
  bool is_test;         // Authenticity == 'T'
  bool round_lots_only;
  char issue_classification;
  // pad to taste; keep it small
};