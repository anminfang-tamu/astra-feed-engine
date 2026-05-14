#pragma once

#include "replay/MdEvent.hpp"
#include "replay/SymbolTable.hpp"

#include <cstdint>
#include <string>
#include <string_view>

class NyseTaqParser {
public:
  static constexpr int64_t kPriceScale = 10000;

  explicit NyseTaqParser(SymbolTable &symbols, uint8_t channel_id = 0);

  bool parseLine(std::string_view line, MdEvent &event);

  void setChannelId(uint8_t channel_id);
  uint8_t channelId() const;
  bool lastLineSkipped() const;
  const std::string &lastError() const;

  static bool parsePriceTicks(std::string_view value, int64_t &price_ticks);
  static bool parseSourceTimeNs(std::string_view value, uint64_t &ns);

private:
  SymbolTable &symbols_;
  uint8_t channel_id_{0};
  bool last_line_skipped_{false};
  std::string last_error_;
};
