#pragma once

#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelPhase.hpp"
#include "astra/metrics/DecodeStageTiming.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Parses NASDAQ ITCH 5.0 messages and applies them directly to BookManager.
// Stock Locate (bytes 1-2 of every message) is used as the symbol ID —
// no secondary symbol-table lookup needed on the hot path.
class ItchParser {
public:
  static constexpr int64_t kPriceScale =
      10000; // ITCH price unit = 1/10000 dollar

  explicit ItchParser(astra::symbol::StockDirectory &symbols,
                      BookManager &books, uint8_t channel_id = 0);

  // Parse one raw ITCH 5.0 message and apply to the book.
  // Returns true if the message produced a book update, false if skipped/error.
  bool handleMessage(std::span<const std::byte> message);

  void reset();
  void setChannelId(uint8_t channel_id);
  // void    setStageTiming(DecodeStageTiming *timing) noexcept;
  uint8_t channelId() const;
  ChannelPhase channelPhase() const noexcept;
  bool bookUniverseReady() const noexcept { return book_universe_ready_; }
  bool lastMessageSkipped() const;
  const std::string &lastError() const;

private:
  void handleAdd(std::span<const std::byte> msg, uint16_t locate,
                 bool with_mpid);
  void handleExecution(std::span<const std::byte> msg, uint16_t locate,
                       bool with_price);
  void handleCancel(std::span<const std::byte> msg, uint16_t locate);
  void handleDelete(std::span<const std::byte> msg, uint16_t locate);
  void handleReplace(std::span<const std::byte> msg, uint16_t locate);
  void handleBrokenTrade(std::span<const std::byte> msg, uint16_t locate);
  void handleStockDirectory(std::span<const std::byte> msg);
  void handleSystemEvent(std::span<const std::byte> msg);
  bool handleSystemHoursStart() noexcept;
  void markStartupAdminMessage(char type) noexcept;
  bool createRegisteredBook(uint16_t locate) noexcept;
  bool createRegisteredBooks() noexcept;

  bool skip();
  bool fail(std::string error);
  // uint64_t timingStart() const noexcept;
  // void recordBookTiming(uint64_t start_ticks) noexcept;

  static uint16_t readU16BE(std::span<const std::byte> msg, std::size_t off);
  static uint32_t readU32BE(std::span<const std::byte> msg, std::size_t off);
  static uint64_t readU64BE(std::span<const std::byte> msg, std::size_t off);

  astra::symbol::StockDirectory &symbols_;
  BookManager &books_;
  uint8_t channel_id_{0};
  ChannelPhase channel_phase_{ChannelPhase::WaitingStartOfMessages};
  bool book_universe_ready_{false};
  bool last_message_skipped_{false};
  std::string last_error_;
  std::array<bool, astra::symbol::StockDirectory::kMaxLocate>
      prepared_book_by_locate_{};
  // DecodeStageTiming *timing_{nullptr};
};
