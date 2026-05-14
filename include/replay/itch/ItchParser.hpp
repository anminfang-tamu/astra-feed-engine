#pragma once

#include "replay/MdEvent.hpp"
#include "replay/SymbolTable.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

class ItchParser {
public:
  static constexpr int64_t kPriceScale = 10000;

  explicit ItchParser(SymbolTable &symbols, uint8_t channel_id = 0);

  bool parseMessage(std::span<const std::byte> message, MdEvent &event);
  void reset();

  void setChannelId(uint8_t channel_id);
  uint8_t channelId() const;
  bool lastMessageSkipped() const;
  const std::string &lastError() const;
  std::size_t openOrderCount() const;

private:
  struct OrderState {
    uint32_t symbol_id{0};
    char side{'\0'};
    int64_t price_ticks{0};
    uint32_t qty{0};
  };

  struct ExecutionState {
    uint64_t order_id{0};
    uint32_t symbol_id{0};
    char side{'\0'};
    int64_t price_ticks{0};
    uint32_t qty{0};
  };

  bool parseAdd(std::span<const std::byte> message, MdEvent &event,
                bool has_attribution, uint64_t sequence);
  bool parseExecution(std::span<const std::byte> message, MdEvent &event,
                      bool has_execution_price, uint64_t sequence);
  bool parseCancel(std::span<const std::byte> message, MdEvent &event,
                   uint64_t sequence);
  bool parseDelete(std::span<const std::byte> message, MdEvent &event,
                   uint64_t sequence);
  bool parseReplace(std::span<const std::byte> message, MdEvent &event,
                    uint64_t sequence);
  bool parseBrokenTrade(std::span<const std::byte> message, MdEvent &event,
                        uint64_t sequence);
  bool parseStockDirectory(std::span<const std::byte> message);

  void fillBaseEvent(MdEvent &event, uint64_t sequence, uint64_t timestamp_ns,
                     uint32_t symbol_id) const;
  bool skip();
  bool fail(std::string error);

  SymbolTable &symbols_;
  uint8_t channel_id_{0};
  uint64_t message_seq_no_{0};
  bool last_message_skipped_{false};
  std::string last_error_;
  std::unordered_map<uint64_t, OrderState> orders_;
  std::unordered_map<uint64_t, ExecutionState> executions_by_match_;
};
