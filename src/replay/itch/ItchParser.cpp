#include "replay/itch/ItchParser.hpp"
#include "astra/core/Time.hpp"
#include "astra/utils/DebugTrace.hpp"

#include <cstring>
#include <string_view>

namespace {

constexpr std::size_t kStockOffset = 24; // offset of 8-char ticker in Add messages
constexpr std::size_t kStockLen    = 8;

std::string_view fixedText(std::span<const std::byte> msg,
                            std::size_t off, std::size_t len) {
  const auto *p = reinterpret_cast<const char *>(msg.data() + off);
  std::size_t n = len;
  while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\0')) --n;
  return {p, n};
}

bool parseSide(uint8_t raw, OrderSide &side) {
  if (raw == 'B') { side = OrderSide::Buy;  return true; }
  if (raw == 'S') { side = OrderSide::Sell; return true; }
  return false;
}

} // namespace

ItchParser::ItchParser(SymbolTable &symbols, BookManager &books, uint8_t channel_id)
    : symbols_(symbols), books_(books), channel_id_(channel_id),
      orders_(kOrderStateCapacity), executions_by_match_(kExecutionCapacity) {}

uint16_t ItchParser::readU16BE(std::span<const std::byte> msg, std::size_t off) {
  return (static_cast<uint16_t>(std::to_integer<uint8_t>(msg[off])) << 8) |
          static_cast<uint16_t>(std::to_integer<uint8_t>(msg[off + 1]));
}

uint32_t ItchParser::readU32BE(std::span<const std::byte> msg, std::size_t off) {
  return (static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off]))     << 24) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off + 1])) << 16) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off + 2])) <<  8) |
          static_cast<uint32_t>(std::to_integer<uint8_t>(msg[off + 3]));
}

uint64_t ItchParser::readU64BE(std::span<const std::byte> msg, std::size_t off) {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v = (v << 8) | std::to_integer<uint8_t>(msg[off + i]);
  return v;
}

bool ItchParser::handleMessage(std::span<const std::byte> msg) {
  last_error_.clear();
  last_message_skipped_ = false;

  if (msg.empty()) {
    ASTRA_TRACE("itch empty message");
    return fail("empty ITCH message");
  }

  const char     type   = static_cast<char>(std::to_integer<uint8_t>(msg[0]));
  const uint16_t locate = msg.size() >= 3 ? readU16BE(msg, 1) : 0;
  ASTRA_TRACE("itch handle type=%c locate=%u size=%zu", type, locate,
              msg.size());

  switch (type) {
  // Book-relevant messages
  case 'A': handleAdd(msg, locate, false);        return !last_message_skipped_;
  case 'F': handleAdd(msg, locate, true);         return !last_message_skipped_;
  case 'E': handleExecution(msg, locate, false);  return !last_message_skipped_;
  case 'C': handleExecution(msg, locate, true);   return !last_message_skipped_;
  case 'X': handleCancel(msg, locate);            return !last_message_skipped_;
  case 'D': handleDelete(msg, locate);            return !last_message_skipped_;
  case 'U': handleReplace(msg, locate);           return !last_message_skipped_;
  case 'B': handleBrokenTrade(msg, locate);       return !last_message_skipped_;
  case 'R': handleStockDirectory(msg);            return false;
  // System / non-book messages — skip silently
  default:  return skip();
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
void ItchParser::handleAdd(std::span<const std::byte> msg, uint16_t locate, bool with_mpid) {
  const std::size_t required = with_mpid ? 40 : 36;
  if (msg.size() < required) {
    ASTRA_TRACE("itch add truncated locate=%u size=%zu required=%zu", locate,
                msg.size(), required);
    return (void)fail("truncated add order");
  }

  OrderSide side{};
  if (!parseSide(std::to_integer<uint8_t>(msg[19]), side)) {
    ASTRA_TRACE("itch add invalid side locate=%u raw=%u", locate,
                std::to_integer<uint8_t>(msg[19]));
    return (void)fail("invalid side in add order");
  }

  const uint64_t order_id    = readU64BE(msg, 11);
  const uint32_t qty         = readU32BE(msg, 20);
  const uint64_t price       = static_cast<uint64_t>(readU32BE(msg, 32));
  ASTRA_TRACE("itch add locate=%u order=%llu side=%c qty=%u price=%llu",
              locate, static_cast<unsigned long long>(order_id),
              side == OrderSide::Buy ? 'B' : 'S', qty,
              static_cast<unsigned long long>(price));

  // Register ticker as a fallback if Stock Directory hasn't arrived for this locate.
  const std::string_view t = fixedText(msg, kStockOffset, kStockLen);
  if (!t.empty()) {
    symbols_.setTicker(locate, t);
    books_.setSymbolTier(locate, astra::capacity::tierForTicker(t));
  }

  orders_.insertOrAssign(order_id,
                         {side == OrderSide::Buy ? 'B' : 'S', price, qty});
  ASTRA_TRACE("itch add route book locate=%u order=%llu",
              locate, static_cast<unsigned long long>(order_id));
  const uint64_t book_start_ns = timingStart();
  books_.addOrder(locate, order_id, price, qty, side, channel_id_);
  recordBookTiming(book_start_ns);
  ASTRA_TRACE("itch add done locate=%u order=%llu",
              locate, static_cast<unsigned long long>(order_id));
}

// 'E' (31 bytes) / 'C' (36 bytes)
// [11-18] Order Reference Number
// [19-22] Executed Shares
// [23-30] Match Number
// [31]    Printable (C only)
// [32-35] Execution Price (C only)
void ItchParser::handleExecution(std::span<const std::byte> msg, uint16_t locate, bool with_price) {
  const std::size_t required = with_price ? 36 : 31;
  if (msg.size() < required) {
    ASTRA_TRACE("itch execution truncated locate=%u size=%zu required=%zu",
                locate, msg.size(), required);
    return (void)fail("truncated execution");
  }

  const uint64_t order_id      = readU64BE(msg, 11);
  const uint32_t executed_qty  = readU32BE(msg, 19);
  const uint64_t match_number  = readU64BE(msg, 23);
  ASTRA_TRACE("itch execution locate=%u order=%llu qty=%u match=%llu with_price=%d",
              locate, static_cast<unsigned long long>(order_id), executed_qty,
              static_cast<unsigned long long>(match_number),
              with_price ? 1 : 0);

  const OrderState *order = orders_.find(order_id);
  if (order == nullptr) {
    ASTRA_TRACE("itch execution unknown order locate=%u order=%llu", locate,
                static_cast<unsigned long long>(order_id));
    return (void)skip();
  }

  // Record match for potential BrokenTrade reversal
  const uint64_t exec_price = with_price
      ? static_cast<uint64_t>(readU32BE(msg, 32))
      : order->price;
  const OrderSide exec_side = order->side == 'B' ? OrderSide::Buy : OrderSide::Sell;
  executions_by_match_.insertOrAssign(
      match_number, {order_id, executed_qty, exec_price, exec_side});

  const uint64_t book_start_ns = timingStart();
  books_.trade(locate, order_id, executed_qty);
  recordBookTiming(book_start_ns);
  ASTRA_TRACE("itch execution book trade done locate=%u order=%llu",
              locate, static_cast<unsigned long long>(order_id));

  OrderState *mutable_order = orders_.find(order_id);
  if (mutable_order != nullptr) {
    if (executed_qty >= mutable_order->qty)
      orders_.erase(order_id);
    else
      mutable_order->qty -= executed_qty;
  }
}

// 'X' (23 bytes)
// [11-18] Order Reference Number
// [19-22] Cancelled Shares
void ItchParser::handleCancel(std::span<const std::byte> msg, uint16_t locate) {
  if (msg.size() < 23) {
    ASTRA_TRACE("itch cancel truncated locate=%u size=%zu", locate, msg.size());
    return (void)fail("truncated cancel");
  }

  const uint64_t order_id     = readU64BE(msg, 11);
  const uint32_t canceled_qty = readU32BE(msg, 19);
  ASTRA_TRACE("itch cancel locate=%u order=%llu qty=%u", locate,
              static_cast<unsigned long long>(order_id), canceled_qty);

  const uint64_t book_start_ns = timingStart();
  books_.cancelShares(locate, order_id, canceled_qty);
  recordBookTiming(book_start_ns);
  ASTRA_TRACE("itch cancel book done locate=%u order=%llu", locate,
              static_cast<unsigned long long>(order_id));

  OrderState *order = orders_.find(order_id);
  if (order != nullptr) {
    if (canceled_qty >= order->qty)
      orders_.erase(order_id);
    else
      order->qty -= canceled_qty;
  }
}

// 'D' (19 bytes)
// [11-18] Order Reference Number
void ItchParser::handleDelete(std::span<const std::byte> msg, uint16_t locate) {
  if (msg.size() < 19) {
    ASTRA_TRACE("itch delete truncated locate=%u size=%zu", locate, msg.size());
    return (void)fail("truncated delete");
  }

  const uint64_t order_id = readU64BE(msg, 11);
  ASTRA_TRACE("itch delete locate=%u order=%llu", locate,
              static_cast<unsigned long long>(order_id));
  orders_.erase(order_id);
  const uint64_t book_start_ns = timingStart();
  books_.deleteOrder(locate, order_id);
  recordBookTiming(book_start_ns);
  ASTRA_TRACE("itch delete book done locate=%u order=%llu", locate,
              static_cast<unsigned long long>(order_id));
}

// 'U' (35 bytes)
// [11-18] Original Order Reference Number
// [19-26] New Order Reference Number
// [27-30] Shares
// [31-34] Price
void ItchParser::handleReplace(std::span<const std::byte> msg, uint16_t locate) {
  if (msg.size() < 35) {
    ASTRA_TRACE("itch replace truncated locate=%u size=%zu", locate,
                msg.size());
    return (void)fail("truncated replace");
  }

  const uint64_t old_id    = readU64BE(msg, 11);
  const uint64_t new_id    = readU64BE(msg, 19);
  const uint32_t new_qty   = readU32BE(msg, 27);
  const uint64_t new_price = static_cast<uint64_t>(readU32BE(msg, 31));
  ASTRA_TRACE("itch replace locate=%u old=%llu new=%llu qty=%u price=%llu",
              locate, static_cast<unsigned long long>(old_id),
              static_cast<unsigned long long>(new_id), new_qty,
              static_cast<unsigned long long>(new_price));

  // Update our order-state cache: old entry → new entry (same side)
  const OrderState *old_state = orders_.find(old_id);
  if (old_state != nullptr) {
    orders_.insertOrAssign(new_id, {old_state->side, new_price, new_qty});
    orders_.erase(old_id);
  }

  const uint64_t book_start_ns = timingStart();
  books_.replaceOrder(locate, old_id, new_id, new_price, new_qty);
  recordBookTiming(book_start_ns);
  ASTRA_TRACE("itch replace book done locate=%u old=%llu new=%llu", locate,
              static_cast<unsigned long long>(old_id),
              static_cast<unsigned long long>(new_id));
}

// 'B' (19 bytes)
// [11-18] Match Number
void ItchParser::handleBrokenTrade(std::span<const std::byte> msg, uint16_t locate) {
  if (msg.size() < 19) {
    ASTRA_TRACE("itch broken truncated locate=%u size=%zu", locate, msg.size());
    return (void)fail("truncated broken trade");
  }

  const uint64_t match_number = readU64BE(msg, 11);
  ASTRA_TRACE("itch broken locate=%u match=%llu", locate,
              static_cast<unsigned long long>(match_number));
  const MatchEntry *entry = executions_by_match_.find(match_number);
  if (entry == nullptr) {
    ASTRA_TRACE("itch broken unknown match locate=%u match=%llu", locate,
                static_cast<unsigned long long>(match_number));
    return (void)skip();
  }

  const MatchEntry &m = *entry;
  const uint64_t book_start_ns = timingStart();
  books_.reverseExecution(locate, m.order_id, m.price, m.executed_qty, m.side);
  recordBookTiming(book_start_ns);

  // Restore order-state cache if fully executed order came back
  if (orders_.find(m.order_id) == nullptr) {
    orders_.insertOrAssign(
        m.order_id, {m.side == OrderSide::Buy ? 'B' : 'S', m.price,
                     m.executed_qty});
  } else {
    OrderState *order = orders_.find(m.order_id);
    if (order != nullptr) order->qty += m.executed_qty;
  }

  executions_by_match_.erase(match_number);
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
  if (msg.size() < 39) return;
  const uint16_t      locate = readU16BE(msg, 1);
  const std::string_view sym = fixedText(msg, 11, kStockLen);
  if (sym.empty() || locate == 0) return;
  ASTRA_TRACE("itch stock_directory locate=%u symbol=%.*s", locate,
              static_cast<int>(sym.size()), sym.data());

  SymbolInfo info{};
  const std::size_t n = sym.size() < 8 ? sym.size() : 8;
  std::memcpy(info.ticker, sym.data(), n);
  info.ticker[n]             = '\0';
  info.locate                = locate;
  info.market_category       = static_cast<char>(std::to_integer<uint8_t>(msg[19]));
  info.fin_status            = static_cast<char>(std::to_integer<uint8_t>(msg[20]));
  info.round_lot_size        = readU32BE(msg, 21);
  info.round_lots_only       = std::to_integer<uint8_t>(msg[25]) == 'Y';
  info.issue_classification  = static_cast<char>(std::to_integer<uint8_t>(msg[26]));
  info.is_test               = std::to_integer<uint8_t>(msg[29]) == 'T';
  symbols_.set(locate, info);
  books_.setSymbolTier(locate, astra::capacity::tierForTicker(sym));
  books_.registerStockLocate(locate, channel_id_);
}

void ItchParser::reset() {
  last_message_skipped_ = false;
  last_error_.clear();
  orders_.clear();
  executions_by_match_.clear();
}

void    ItchParser::setChannelId(uint8_t id) { channel_id_ = id; }
void    ItchParser::setStageTiming(DecodeStageTiming *timing) noexcept {
  timing_ = timing;
}
uint8_t ItchParser::channelId()        const  { return channel_id_; }
bool    ItchParser::lastMessageSkipped() const { return last_message_skipped_; }
const std::string &ItchParser::lastError() const { return last_error_; }

bool ItchParser::skip() { last_message_skipped_ = true; return false; }
bool ItchParser::fail(std::string error) { last_error_ = std::move(error); return false; }

uint64_t ItchParser::timingStart() const noexcept {
  return timing_ != nullptr ? nowNs() : 0;
}

void ItchParser::recordBookTiming(uint64_t start_ns) noexcept {
  if (timing_ == nullptr)
    return;

  const uint64_t end_ns = nowNs();
  if (end_ns >= start_ns)
    timing_->book_ns += end_ns - start_ns;
  ++timing_->book_messages;
}
