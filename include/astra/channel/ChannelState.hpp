#pragma once

#include "astra/channel/ChannelHealth.hpp"
#include "astra/sequencing/GapBuffer.hpp"
#include "astra/symbol/StockLocate.hpp"

#include <array>
#include <cstdint>
#include <cstring>

struct ChannelState {
  static constexpr uint16_t kMaxStockLocates = astra::symbol::kMaxStockLocate;
  static constexpr std::size_t kSessionBytes = 10;

  uint16_t channel_id{0};
  char session[kSessionBytes + 1]{};

  uint64_t next_expected_seq = 1;
  ChannelHealth status = ChannelHealth::Good;

  // out-of-order packets waiting for missing seqs
  GapBuffer gap_buffer;

  std::array<uint16_t, kMaxStockLocates> stock_locates{};
  std::array<bool, kMaxStockLocates> stock_locate_seen{};
  uint16_t stock_locate_count{0};

  void setSession(const char *session_bytes) noexcept {
    if (session_bytes == nullptr) {
      session[0] = '\0';
      return;
    }
    std::memcpy(session, session_bytes, kSessionBytes);
    session[kSessionBytes] = '\0';
  }

  bool registerStockLocate(uint16_t locate) noexcept {
    if (!astra::symbol::isValidStockLocate(locate)) return false;
    if (stock_locate_seen[locate]) return true;
    if (stock_locate_count >= stock_locates.size()) return false;
    stock_locate_seen[locate] = true;
    stock_locates[stock_locate_count++] = locate;
    return true;
  }
};
