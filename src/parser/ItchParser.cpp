#include "astra/parser/ItchParser.hpp"
#include "astra/book/BookCapacity.hpp"
#include "astra/core/Time.hpp"

#include <cstring>
#include <iostream>
#include <string_view>

namespace {

constexpr std::size_t kStockOffset =
    24; // offset of 8-char ticker in Add messages
constexpr std::size_t kStockLen = 8;
constexpr std::size_t kSystemEventLen = 12;
constexpr std::size_t kSystemEventCodeOffset = 11;

std::string_view fixedText(std::span<const std::byte> msg, std::size_t off,
                           std::size_t len) {
  const auto *p = reinterpret_cast<const char *>(msg.data() + off);
  std::size_t n = len;
  while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\0'))
    --n;
  return {p, n};
}

} // namespace

ItchParser::ItchParser(astra::symbol::StockDirectory &symbols,
                       BookManager &books, uint8_t channel_id)
    : symbols_(symbols), books_(books), channel_id_(channel_id) {}

uint16_t ItchParser::readU16BE(std::span<const std::byte> msg,
                               std::size_t off) {
  return (static_cast<uint16_t>(std::to_integer<uint8_t>(msg[off])) << 8) |
         static_cast<uint16_t>(std::to_integer<uint8_t>(msg[off + 1]));
}

uint32_t ItchParser::readU32BE(std::span<const std::byte> msg,
                               std::size_t off) {
  return (static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off])) << 24) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off + 1])) << 16) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off + 2])) << 8) |
         static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off + 3]));
}

uint64_t ItchParser::readU64BE(std::span<const std::byte> msg,
                               std::size_t off) {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v = (v << 8) | std::to_integer<uint8_t>(msg[off + i]);
  return v;
}

bool ItchParser::handleMessage(std::span<const std::byte> msg) {
  last_error_.clear();
  last_message_skipped_ = false;

  if (msg.empty()) {
    return fail("empty ITCH message");
  }

  const char type = static_cast<char>(std::to_integer<uint8_t>(msg[0]));
  const uint16_t locate = msg.size() >= 3 ? readU16BE(msg, 1) : 0;

  switch (type) {
  // Book-relevant messages
  case 'A':
    handleAdd(msg, locate, false);
    return !last_message_skipped_;
  case 'F':
    handleAdd(msg, locate, true);
    return !last_message_skipped_;
  case 'E':
    handleExecution(msg, locate, false);
    return !last_message_skipped_;
  case 'C':
    handleExecution(msg, locate, true);
    return !last_message_skipped_;
  case 'X':
    handleCancel(msg, locate);
    return !last_message_skipped_;
  case 'D':
    handleDelete(msg, locate);
    return !last_message_skipped_;
  case 'U':
    handleReplace(msg, locate);
    return !last_message_skipped_;
  case 'B':
    handleBrokenTrade(msg, locate);
    return !last_message_skipped_;
  case 'R':
    handleStockDirectory(msg);
    return false;
  case 'S':
    handleSystemEvent(msg);
    return false;
  case 'H': // Stock Trading Action
  case 'Y': // Reg SHO Restriction
  case 'L': // Market Participant Position
  case 'V': // MWCB Decline Level
  case 'W': // MWCB Status
  case 'K': // IPO Quoting Period Update
  case 'J': // LULD Auction Collar
  case 'h': // Operational Halt
    markStartupAdminMessage(type);
    return false;
  // System / non-book messages — skip silently
  default:
    return skip();
  }
}

// 'A' (36 bytes) / 'F' (40 bytes)
// [0]    type
// [1-2]  Stock Locate
// [3-4]  Tracking Number
// [5-10] Timestamp (6 bytes)
// [11-18] Order Reference Number
// [19]   Buy/Sell Indicator
// [20-23] Shares
// [24-31] Stock
// [32-35] Price
// [36-39] Attribution (F only)
void ItchParser::handleAdd(std::span<const std::byte> msg, uint16_t locate,
                           bool with_mpid) {
  const std::size_t required = with_mpid ? 40 : 36;
  if (msg.size() < required) {
    return (void)fail("truncated add order");
  }

  const uint64_t order_id = readU64BE(msg, 11);
  const uint32_t qty = readU32BE(msg, 20);
  const uint64_t price = static_cast<uint64_t>(readU32BE(msg, 32));

  if (!symbols_.isRegistered(locate)) {
    return (void)skip();
  }

  books_.addOrder(locate, order_id, price, qty, (char)msg[19]);
}

// 'E' (31 bytes) / 'C' (36 bytes)
// [11-18] Order Reference Number
// [19-22] Executed Shares
// [23-30] Match Number
// [31]    Printable (C only)
// [32-35] Execution Price (C only)
void ItchParser::handleExecution(std::span<const std::byte> msg,
                                 uint16_t locate, bool with_price) {
  const std::size_t required = with_price ? 36 : 31;
  if (msg.size() < required) {
    return (void)fail("truncated execution");
  }

  const uint64_t order_id = readU64BE(msg, 11);
  const uint32_t executed_qty = readU32BE(msg, 19);

  if (!books_.trade(locate, order_id, executed_qty)) {
    return (void)skip();
  }
  // Execution part will be implemented later
}

// 'X' (23 bytes)
// [11-18] Order Reference Number
// [19-22] Cancelled Shares
void ItchParser::handleCancel(std::span<const std::byte> msg, uint16_t locate) {
  if (msg.size() < 23) {
    return (void)fail("truncated cancel");
  }

  const uint64_t order_id = readU64BE(msg, 11);
  const uint32_t canceled_qty = readU32BE(msg, 19);

  books_.cancelShares(locate, order_id, canceled_qty);
}

// 'D' (19 bytes)
// [11-18] Order Reference Number
void ItchParser::handleDelete(std::span<const std::byte> msg, uint16_t locate) {
  if (msg.size() < 19) {
    return (void)fail("truncated delete");
  }

  const uint64_t order_id = readU64BE(msg, 11);
  books_.deleteOrder(locate, order_id);
}

// 'U' (35 bytes)
// [11-18] Original Order Reference Number
// [19-26] New Order Reference Number
// [27-30] Shares
// [31-34] Price
void ItchParser::handleReplace(std::span<const std::byte> msg,
                               uint16_t locate) {
  if (msg.size() < 35) {
    return (void)fail("truncated replace");
  }

  const uint64_t old_id = readU64BE(msg, 11);
  const uint64_t new_id = readU64BE(msg, 19);
  const uint32_t new_qty = readU32BE(msg, 27);
  const uint64_t new_price = static_cast<uint64_t>(readU32BE(msg, 31));

  books_.replaceOrder(locate, old_id, new_id, new_price, new_qty);
}

// 'B' (19 bytes)
// [11-18] Match Number
void ItchParser::handleBrokenTrade(std::span<const std::byte> msg, uint16_t) {
  if (msg.size() < 19) {
    return (void)fail("truncated broken trade");
  }

  // Match-number history is not tracked in the parser.
  return (void)skip();
}

// 'R' (39 bytes) — Stock Directory
// [1-2]  Stock Locate
// [11-18] Stock (8 chars)
// [19]   Market Category
// [20]   Financial Status Indicator
// [21-24] Round Lot Size
// [25]   Round Lots Only ('Y'/'N')
// [26]   Issue Classification
// [29]   Authenticity ('P'=live, 'T'=test)
void ItchParser::handleStockDirectory(std::span<const std::byte> msg) {
  if (msg.size() < 39)
    return;
  const uint16_t locate = readU16BE(msg, 1);
  const std::string_view sym = fixedText(msg, 11, kStockLen);
  if (sym.empty() || locate == 0)
    return;
  if (channel_phase_ == ChannelPhase::WaitingStartOfMessages) {
    channel_phase_ = ChannelPhase::StartupDirectorySpin;
  }
  std::cout << "ITCH R StockDirectory channel="
            << static_cast<unsigned>(channel_id_) << " locate=" << locate
            << " ticker=" << sym << "\n";

  astra::symbol::StockDirectoryEntry entry{};
  const std::size_t n = sym.size() < 8 ? sym.size() : 8;
  std::memcpy(entry.ticker, sym.data(), n);
  entry.ticker[n] = '\0';
  entry.locate = locate;
  entry.market_category = static_cast<char>(std::to_integer<uint8_t>(msg[19]));
  entry.fin_status = static_cast<char>(std::to_integer<uint8_t>(msg[20]));
  entry.round_lot_size = readU32BE(msg, 21);
  entry.round_lots_only = std::to_integer<uint8_t>(msg[25]) == 'Y';
  entry.issue_classification =
      static_cast<char>(std::to_integer<uint8_t>(msg[26]));
  entry.is_test = std::to_integer<uint8_t>(msg[29]) == 'T';
  symbols_.set(locate, entry);
  books_.setBookCapacityTier(locate, astra::book_capacity::tierForTicker(sym));
}

// 'S' (12 bytes) — System Event
// [11] Event Code:
//      'O' Start of Messages, 'S' Start of System Hours,
//      'Q' Start of Market Hours, 'M' End of Market Hours,
//      'E' End of System Hours, 'C' End of Messages.
void ItchParser::handleSystemEvent(std::span<const std::byte> msg) {
  if (msg.size() < kSystemEventLen) {
    return (void)fail("truncated system event");
  }

  const char event_code =
      static_cast<char>(std::to_integer<uint8_t>(msg[kSystemEventCodeOffset]));
  switch (event_code) {
  case 'O':
    channel_phase_ = ChannelPhase::StartupDirectorySpin;
    break;
  case 'S':
    std::cout << "ITCH SS SystemHoursStart channel="
              << static_cast<unsigned>(channel_id_)
              << " registered_symbols=" << symbols_.size() << "\n";
    handleSystemHoursStart();
    break;
  case 'Q':
    channel_phase_ = ChannelPhase::MarketHours;
    break;
  case 'M':
    channel_phase_ = ChannelPhase::SystemHours;
    break;
  case 'E':
  case 'C':
    channel_phase_ = ChannelPhase::WaitingStartOfMessages;
    break;
  default:
    markStartupAdminMessage(event_code);
    break;
  }
}

void ItchParser::handleSystemHoursStart() noexcept {
  channel_phase_ = ChannelPhase::SystemHours;
  createRegisteredBooks();
}

void ItchParser::markStartupAdminMessage(char type) noexcept {
  (void)type;
  if (channel_phase_ == ChannelPhase::StartupDirectorySpin) {
    channel_phase_ = ChannelPhase::StartupAdminSpin;
  }
}

void ItchParser::createRegisteredBook(uint16_t locate) noexcept {
  if (!astra::symbol::isValidStockLocate(locate) ||
      prepared_book_by_locate_[locate] || !symbols_.isRegistered(locate)) {
    return;
  }

  (void)books_.getOrCreate(locate);
  prepared_book_by_locate_[locate] = true;
}

void ItchParser::createRegisteredBooks() noexcept {
  for (uint16_t locate = 1; locate < astra::symbol::StockDirectory::kMaxLocate;
       ++locate) {
    createRegisteredBook(locate);
  }
}

void ItchParser::reset() {
  channel_phase_ = ChannelPhase::WaitingStartOfMessages;
  last_message_skipped_ = false;
  last_error_.clear();
  prepared_book_by_locate_.fill(false);
}

void ItchParser::setChannelId(uint8_t id) { channel_id_ = id; }
// void    ItchParser::setStageTiming(DecodeStageTiming *timing) noexcept {
//   timing_ = timing;
// }
uint8_t ItchParser::channelId() const { return channel_id_; }
ChannelPhase ItchParser::channelPhase() const noexcept {
  return channel_phase_;
}
bool ItchParser::lastMessageSkipped() const { return last_message_skipped_; }
const std::string &ItchParser::lastError() const { return last_error_; }

bool ItchParser::skip() {
  last_message_skipped_ = true;
  return false;
}
bool ItchParser::fail(std::string error) {
  last_error_ = std::move(error);
  return false;
}
