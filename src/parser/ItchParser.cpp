#include "astra/parser/ItchParser.hpp"
#include "astra/protocol/ItchMessageLength.hpp"

#include <cstring>
#include <string_view>

namespace {

constexpr std::size_t kStockLen = 8;
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

  const std::uint8_t expected_length =
      astra::protocol::expectedItchMessageLength(msg[0]);
  if (expected_length == 0)
    return fail("unsupported ITCH message type");
  if (msg.size() != expected_length)
    return fail("invalid ITCH message length");
  return dispatchMessage(msg);
}

bool ItchParser::handlePrevalidatedMessage(
    std::span<const std::byte> msg) {
  last_error_.clear();
  last_message_skipped_ = false;
  if (msg.empty())
    return fail("empty prevalidated ITCH message");
  return dispatchMessage(msg);
}

bool ItchParser::dispatchMessage(std::span<const std::byte> msg) {
  const char type = static_cast<char>(std::to_integer<uint8_t>(msg[0]));
  const uint16_t locate = readU16BE(msg, 1);

  if (channel_phase_ == ChannelPhase::EndOfMessages) {
    return fail("ITCH message after end of messages");
  }

  switch (type) {
  // Book-relevant messages
  case 'A':
    handleAdd(msg, locate);
    return !last_message_skipped_;
  case 'F':
    handleAdd(msg, locate);
    return !last_message_skipped_;
  case 'E':
    handleOrderExecuted(msg, locate);
    return !last_message_skipped_;
  case 'C':
    handleOrderExecutedWithPrice(msg, locate);
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
void ItchParser::handleAdd(std::span<const std::byte> msg, uint16_t locate) {

  const uint64_t order_id = readU64BE(msg, 11);
  const uint32_t qty = readU32BE(msg, 20);
  const uint64_t price = static_cast<uint64_t>(readU32BE(msg, 32));

  // The manager's resident descriptor is the authoritative preparation bit.
  // Avoid separate directory and parser-side flag reads on every hot add.
  applyBookResult(books_.addOrder(locate, order_id, price, qty,
                                  static_cast<char>(
                                      std::to_integer<uint8_t>(msg[19]))),
                  "add order");
}

// 'E' (31 bytes) / 'C' (36 bytes)
// [11-18] Order Reference Number
// [19-22] Executed Shares
// [23-30] Match Number
// [31]    Printable (C only)
// [32-35] Execution Price (C only)
void ItchParser::handleOrderExecuted(std::span<const std::byte> msg,
                                     uint16_t locate) {
  applyExecution(msg, locate);
}

void ItchParser::handleOrderExecutedWithPrice(
    std::span<const std::byte> msg, uint16_t locate) {
  applyExecution(msg, locate);
}

void ItchParser::applyExecution(std::span<const std::byte> msg,
                                uint16_t locate) {
  const uint64_t order_id = readU64BE(msg, 11);
  const uint32_t executed_qty = readU32BE(msg, 19);

  applyBookResult(books_.executeOrder(locate, order_id, executed_qty),
                  "order execution");
}

// 'X' (23 bytes)
// [11-18] Order Reference Number
// [19-22] Cancelled Shares
void ItchParser::handleCancel(std::span<const std::byte> msg, uint16_t locate) {
  const uint64_t order_id = readU64BE(msg, 11);
  const uint32_t canceled_qty = readU32BE(msg, 19);

  applyBookResult(books_.cancelShares(locate, order_id, canceled_qty),
                  "order cancel");
}

// 'D' (19 bytes)
// [11-18] Order Reference Number
void ItchParser::handleDelete(std::span<const std::byte> msg, uint16_t locate) {
  const uint64_t order_id = readU64BE(msg, 11);
  applyBookResult(books_.deleteOrder(locate, order_id), "order delete");
}

// 'U' (35 bytes)
// [11-18] Original Order Reference Number
// [19-26] New Order Reference Number
// [27-30] Shares
// [31-34] Price
void ItchParser::handleReplace(std::span<const std::byte> msg,
                               uint16_t locate) {
  const uint64_t old_id = readU64BE(msg, 11);
  const uint64_t new_id = readU64BE(msg, 19);
  const uint32_t new_qty = readU32BE(msg, 27);
  const uint64_t new_price = static_cast<uint64_t>(readU32BE(msg, 31));

  applyBookResult(
      books_.replaceOrder(locate, old_id, new_id, new_price, new_qty),
      "order replace");
}

// 'B' (19 bytes)
// [11-18] Match Number
void ItchParser::handleBrokenTrade(std::span<const std::byte> msg, uint16_t) {
  (void)msg;
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
  const uint16_t locate = readU16BE(msg, 1);
  const std::string_view sym = fixedText(msg, 11, kStockLen);
  if (sym.empty() || !astra::symbol::isValidStockLocate(locate)) {
    return (void)fail("invalid stock-directory identity");
  }

  // A repeated R is a metadata refresh.  A ticker change for an existing
  // locate is an identity conflict and must not retarget a live order book.
  if (const auto *existing = symbols_.get(locate);
      existing != nullptr && std::string_view(existing->ticker) != sym) {
    return (void)fail("conflicting ticker for stock locate");
  }

  if (channel_phase_ == ChannelPhase::WaitingStartOfMessages) {
    channel_phase_ = ChannelPhase::StartupDirectorySpin;
  }

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
  if (books_prepared_ && !createRegisteredBook(locate)) {
    return (void)fail("unable to prepare stock-directory book");
  }
}

// 'S' (12 bytes) — System Event
// [11] Event Code:
//      'O' Start of Messages, 'S' Start of System Hours,
//      'Q' Start of Market Hours, 'M' End of Market Hours,
//      'E' End of System Hours, 'C' End of Messages.
void ItchParser::handleSystemEvent(std::span<const std::byte> msg) {
  const char event_code =
      static_cast<char>(std::to_integer<uint8_t>(msg[kSystemEventCodeOffset]));
  switch (event_code) {
  case 'O':
    (void)advancePhase(ChannelPhase::StartupDirectorySpin);
    break;
  case 'S':
    handleSystemHoursStart();
    break;
  case 'Q':
    (void)advancePhase(ChannelPhase::MarketHours);
    break;
  case 'M':
    (void)advancePhase(ChannelPhase::PostMarketHours);
    break;
  case 'E':
    (void)advancePhase(ChannelPhase::PostSystemHours);
    break;
  case 'C':
    (void)advancePhase(ChannelPhase::EndOfMessages);
    break;
  default:
    (void)fail("unsupported system event code");
    break;
  }
}

void ItchParser::handleSystemHoursStart() {
  if (!advancePhase(ChannelPhase::SystemHours))
    return;
  if (books_prepared_)
    return;
  books_prepared_ = true;
  if (!createRegisteredBooks()) {
    (void)fail("unable to prepare registered books");
  }
}

bool ItchParser::advancePhase(ChannelPhase next_phase) {
  if (static_cast<unsigned>(next_phase) <
      static_cast<unsigned>(channel_phase_)) {
    return fail("out-of-order system event");
  }
  channel_phase_ = next_phase;
  return true;
}

void ItchParser::markStartupAdminMessage(char type) noexcept {
  (void)type;
  if (channel_phase_ == ChannelPhase::StartupDirectorySpin) {
    channel_phase_ = ChannelPhase::StartupAdminSpin;
  }
}

bool ItchParser::createRegisteredBook(uint16_t locate) noexcept {
  if (!astra::symbol::isValidStockLocate(locate) ||
      !symbols_.isRegistered(locate)) {
    return false;
  }
  if (books_.getOrCreate(locate) == nullptr) {
    return false;
  }
  return true;
}

bool ItchParser::createRegisteredBooks() noexcept {
  bool success = true;
  for (std::size_t slot = 1;
       slot < astra::symbol::StockDirectory::kMaxLocate; ++slot) {
    const auto locate = static_cast<uint16_t>(slot);
    if (symbols_.isRegistered(locate)) {
      success = createRegisteredBook(locate) && success;
    }
  }
  return success;
}

void ItchParser::applyBookResult(astra::book::MutationResult result,
                                 std::string_view operation) {
  if (result == astra::book::MutationResult::Applied)
    return;
  [[unlikely]] applyBookFailure(result, operation);
}

void ItchParser::applyBookFailure(astra::book::MutationResult result,
                                  std::string_view operation) {
  // This is the sole mutation-error formatting boundary. Keeping it cold and
  // non-inlined lets the successful A/F/E/C/X/D/U closure remain strictly
  // allocation-free while preserving a useful terminal decoder error.
  last_message_skipped_ = true;
  last_error_.assign(operation.data(), operation.size());
  last_error_.append(": ");
  last_error_.append(astra::book::mutationResultName(result));
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
bool ItchParser::fail(std::string_view error) {
  // Literal wire/lifecycle failures arrive here without constructing a
  // std::string in the hot parser caller. Any allocation is confined to this
  // explicitly cold boundary.
  last_message_skipped_ = true;
  last_error_.assign(error.data(), error.size());
  return false;
}
