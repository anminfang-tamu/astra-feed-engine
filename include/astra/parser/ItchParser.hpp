#pragma once

#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelPhase.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

class MoldUdpDecoder;

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
  void setChannelId(uint8_t channel_id);
  // void    setStageTiming(DecodeStageTiming *timing) noexcept;
  uint8_t channelId() const;
  ChannelPhase channelPhase() const noexcept;
  bool lastMessageSkipped() const;
  const std::string &lastError() const;

private:
  friend class MoldUdpDecoder;

  // Decoder-only entry after its complete packet-wide type/length prepass.
  // This avoids repeating the 256-entry length-table read on the live path.
  bool handlePrevalidatedMessage(std::span<const std::byte> message);
  bool dispatchMessage(std::span<const std::byte> msg);
  void handleAdd(std::span<const std::byte> msg, uint16_t locate);
  void handleOrderExecuted(std::span<const std::byte> msg, uint16_t locate);
  void handleOrderExecutedWithPrice(std::span<const std::byte> msg,
                                    uint16_t locate);
  void applyExecution(std::span<const std::byte> msg, uint16_t locate);
  void handleCancel(std::span<const std::byte> msg, uint16_t locate);
  void handleDelete(std::span<const std::byte> msg, uint16_t locate);
  void handleReplace(std::span<const std::byte> msg, uint16_t locate);
  void handleBrokenTrade(std::span<const std::byte> msg, uint16_t locate);
  void handleStockDirectory(std::span<const std::byte> msg);
  void handleSystemEvent(std::span<const std::byte> msg);
  void handleSystemHoursStart();
  bool advancePhase(ChannelPhase next_phase);
  void markStartupAdminMessage(char type) noexcept;
  bool createRegisteredBook(uint16_t locate) noexcept;
  bool createRegisteredBooks() noexcept;
  void applyBookResult(astra::book::MutationResult result,
                       std::string_view operation);
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((cold, noinline))
#endif
  void applyBookFailure(astra::book::MutationResult result,
                        std::string_view operation);

  bool skip();
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((cold, noinline))
#endif
  bool fail(std::string_view error);
  // uint64_t timingStart() const noexcept;
  // void recordBookTiming(uint64_t start_ticks) noexcept;

  static uint16_t readU16BE(std::span<const std::byte> msg, std::size_t off);
  static uint32_t readU32BE(std::span<const std::byte> msg, std::size_t off);
  static uint64_t readU64BE(std::span<const std::byte> msg, std::size_t off);

  astra::symbol::StockDirectory &symbols_;
  BookManager &books_;
  uint8_t channel_id_{0};
  ChannelPhase channel_phase_{ChannelPhase::WaitingStartOfMessages};
  bool last_message_skipped_{false};
  std::string last_error_;
  bool books_prepared_{false};
};
