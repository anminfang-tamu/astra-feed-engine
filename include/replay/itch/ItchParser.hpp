#pragma once

#include "astra/book/BookManager.hpp"
#include "astra/metrics/DecodeStageTiming.hpp"
#include "astra/protocol/SymbolTable.hpp"
#include "astra/utils/FixedHashMap.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Parses NASDAQ ITCH 5.0 messages and applies them directly to BookManager.
// Stock Locate (bytes 1-2 of every message) is used as the symbol ID —
// no secondary symbol-table lookup needed on the hot path.
class ItchParser {
public:
  static constexpr int64_t kPriceScale = 10000; // ITCH price unit = 1/10000 dollar

  explicit ItchParser(SymbolTable &symbols, BookManager &books,
                      uint8_t channel_id = 0);

  // Parse one raw ITCH 5.0 message and apply to the book.
  // Returns true if the message produced a book update, false if skipped/error.
  bool handleMessage(std::span<const std::byte> message);

  void    reset();
  void    setChannelId(uint8_t channel_id);
  // void    setStageTiming(DecodeStageTiming *timing) noexcept;
  void    setStockDirectoryWarmup(bool prepare_books,
                                  bool touch_pages) noexcept;
  uint8_t channelId() const;
  bool    lastMessageSkipped() const;
  const std::string &lastError() const;

private:
  static constexpr uint32_t kOrderStateCapacity = 1u << 20;
  static constexpr uint32_t kExecutionCapacity  = 1u << 20;

  // Stored per execution for BrokenTrade ('B') reversal only.
  struct MatchEntry {
    uint64_t  order_id;
    uint32_t  executed_qty;
    uint64_t  price;
    OrderSide side;
  };

  // Minimal order state kept only to record MatchEntry on execution.
  struct OrderState {
    char     side;
    uint64_t price; // price_ticks
    uint32_t qty;
  };

  void handleAdd(std::span<const std::byte> msg, uint16_t locate, bool with_mpid);
  void handleExecution(std::span<const std::byte> msg, uint16_t locate, bool with_price);
  void handleCancel(std::span<const std::byte> msg, uint16_t locate);
  void handleDelete(std::span<const std::byte> msg, uint16_t locate);
  void handleReplace(std::span<const std::byte> msg, uint16_t locate);
  void handleBrokenTrade(std::span<const std::byte> msg, uint16_t locate);
  void handleStockDirectory(std::span<const std::byte> msg);

  bool skip();
  bool fail(std::string error);
  // uint64_t timingStart() const noexcept;
  // void recordBookTiming(uint64_t start_ticks) noexcept;

  static uint16_t readU16BE(std::span<const std::byte> msg, std::size_t off);
  static uint32_t readU32BE(std::span<const std::byte> msg, std::size_t off);
  static uint64_t readU64BE(std::span<const std::byte> msg, std::size_t off);

  SymbolTable  &symbols_;
  BookManager  &books_;
  uint8_t       channel_id_{0};
  bool          last_message_skipped_{false};
  bool          prepare_books_on_directory_{true};
  bool          touch_book_pages_on_directory_{false};
  std::string   last_error_;
  // DecodeStageTiming *timing_{nullptr};

  FixedHashMap<OrderState> orders_;              // for MatchEntry recording
  FixedHashMap<MatchEntry> executions_by_match_; // BrokenTrade
};
