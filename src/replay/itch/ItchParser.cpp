#include "replay/itch/ItchParser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::size_t kCommonTimestampOffset = 5;
constexpr std::size_t kStockSize = 8;

uint8_t loadU8(std::span<const std::byte> message, std::size_t offset) {
  return std::to_integer<uint8_t>(message[offset]);
}

uint32_t loadU32(std::span<const std::byte> message, std::size_t offset) {
  return (static_cast<uint32_t>(loadU8(message, offset)) << 24) |
         (static_cast<uint32_t>(loadU8(message, offset + 1)) << 16) |
         (static_cast<uint32_t>(loadU8(message, offset + 2)) << 8) |
         static_cast<uint32_t>(loadU8(message, offset + 3));
}

uint64_t loadU48(std::span<const std::byte> message, std::size_t offset) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < 6; ++i) {
    value = (value << 8) | loadU8(message, offset + i);
  }
  return value;
}

uint64_t loadU64(std::span<const std::byte> message, std::size_t offset) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | loadU8(message, offset + i);
  }
  return value;
}

std::string_view fixedText(std::span<const std::byte> message,
                           std::size_t offset, std::size_t length) {
  const auto *begin = reinterpret_cast<const char *>(message.data() + offset);
  std::size_t size = length;
  while (size > 0 && (begin[size - 1] == ' ' || begin[size - 1] == '\0')) {
    --size;
  }
  return std::string_view(begin, size);
}

bool parseSide(uint8_t value, char &side) {
  const char ch = static_cast<char>(value);
  if (ch == 'B' || ch == 'S') {
    side = ch;
    return true;
  }
  return false;
}

bool hasSize(std::span<const std::byte> message, std::size_t size) {
  return message.size() >= size;
}

uint64_t timestamp(std::span<const std::byte> message) {
  return loadU48(message, kCommonTimestampOffset);
}

} // namespace

ItchParser::ItchParser(SymbolTable &symbols, uint8_t channel_id)
    : symbols_(symbols), channel_id_(channel_id) {}

bool ItchParser::parseMessage(std::span<const std::byte> message,
                              MdEvent &event) {
  last_error_.clear();
  last_message_skipped_ = false;

  if (message.empty()) {
    return fail("empty ITCH message");
  }

  const uint64_t sequence = ++message_seq_no_;
  const char type = static_cast<char>(loadU8(message, 0));

  switch (type) {
  case 'S':
  case 'H':
  case 'Y':
  case 'L':
  case 'V':
  case 'W':
  case 'K':
  case 'J':
  case 'P':
  case 'Q':
  case 'I':
  case 'N':
    return skip();
  case 'R':
    return parseStockDirectory(message);
  case 'A':
    return parseAdd(message, event, false, sequence);
  case 'F':
    return parseAdd(message, event, true, sequence);
  case 'E':
    return parseExecution(message, event, false, sequence);
  case 'C':
    return parseExecution(message, event, true, sequence);
  case 'X':
    return parseCancel(message, event, sequence);
  case 'D':
    return parseDelete(message, event, sequence);
  case 'U':
    return parseReplace(message, event, sequence);
  case 'B':
    return parseBrokenTrade(message, event, sequence);
  default:
    return skip();
  }
}

void ItchParser::reset() {
  message_seq_no_ = 0;
  last_message_skipped_ = false;
  last_error_.clear();
  orders_.clear();
  executions_by_match_.clear();
}

void ItchParser::setChannelId(uint8_t channel_id) {
  channel_id_ = channel_id;
}

uint8_t ItchParser::channelId() const { return channel_id_; }

bool ItchParser::lastMessageSkipped() const { return last_message_skipped_; }

const std::string &ItchParser::lastError() const { return last_error_; }

std::size_t ItchParser::openOrderCount() const { return orders_.size(); }

bool ItchParser::parseAdd(std::span<const std::byte> message, MdEvent &event,
                          bool has_attribution, uint64_t sequence) {
  const std::size_t required_size = has_attribution ? 40 : 36;
  if (!hasSize(message, required_size)) {
    return fail("truncated add order message");
  }

  char side = '\0';
  if (!parseSide(loadU8(message, 19), side)) {
    return fail("invalid add order side");
  }

  const std::string_view symbol = fixedText(message, 24, kStockSize);
  const uint32_t symbol_id = symbol.empty() ? 0 : symbols_.intern(symbol);
  const uint64_t order_id = loadU64(message, 11);
  const uint32_t qty = loadU32(message, 20);
  const int64_t price_ticks = static_cast<int64_t>(loadU32(message, 32));

  fillBaseEvent(event, sequence, timestamp(message), symbol_id);
  event.type = MdEventType::Add;
  event.side = side;
  event.order_id = order_id;
  event.price_ticks = price_ticks;
  event.qty = qty;

  orders_[order_id] = OrderState{symbol_id, side, price_ticks, qty};
  return true;
}

bool ItchParser::parseExecution(std::span<const std::byte> message,
                                MdEvent &event, bool has_execution_price,
                                uint64_t sequence) {
  const std::size_t required_size = has_execution_price ? 36 : 31;
  if (!hasSize(message, required_size)) {
    return fail("truncated order execution message");
  }

  const uint64_t order_id = loadU64(message, 11);
  auto order = orders_.find(order_id);
  if (order == orders_.end()) {
    return skip();
  }

  const uint32_t executed_qty = loadU32(message, 19);
  const uint64_t match_number = loadU64(message, 23);
  if (executed_qty > order->second.qty) {
    return fail("execution quantity exceeds open order quantity");
  }

  fillBaseEvent(event, sequence, timestamp(message), order->second.symbol_id);
  event.type = MdEventType::Execution;
  event.side = order->second.side;
  event.order_id = order_id;
  event.price_ticks = has_execution_price
                          ? static_cast<int64_t>(loadU32(message, 32))
                          : order->second.price_ticks;
  event.qty = executed_qty;

  executions_by_match_[match_number] =
      ExecutionState{order_id, order->second.symbol_id, order->second.side,
                     order->second.price_ticks, executed_qty};

  order->second.qty -= executed_qty;
  if (order->second.qty == 0) {
    orders_.erase(order);
  }
  return true;
}

bool ItchParser::parseCancel(std::span<const std::byte> message, MdEvent &event,
                             uint64_t sequence) {
  if (!hasSize(message, 23)) {
    return fail("truncated order cancel message");
  }

  const uint64_t order_id = loadU64(message, 11);
  auto order = orders_.find(order_id);
  if (order == orders_.end()) {
    return skip();
  }

  const uint32_t canceled_qty = loadU32(message, 19);
  if (canceled_qty > order->second.qty) {
    return fail("cancel quantity exceeds open order quantity");
  }

  order->second.qty -= canceled_qty;
  fillBaseEvent(event, sequence, timestamp(message), order->second.symbol_id);
  event.type =
      order->second.qty == 0 ? MdEventType::Delete : MdEventType::Modify;
  event.side = order->second.side;
  event.order_id = order_id;
  event.price_ticks = order->second.price_ticks;
  event.qty = order->second.qty;

  if (order->second.qty == 0) {
    orders_.erase(order);
  }
  return true;
}

bool ItchParser::parseDelete(std::span<const std::byte> message, MdEvent &event,
                             uint64_t sequence) {
  if (!hasSize(message, 19)) {
    return fail("truncated order delete message");
  }

  const uint64_t order_id = loadU64(message, 11);
  auto order = orders_.find(order_id);
  if (order == orders_.end()) {
    return skip();
  }

  fillBaseEvent(event, sequence, timestamp(message), order->second.symbol_id);
  event.type = MdEventType::Delete;
  event.side = order->second.side;
  event.order_id = order_id;
  event.price_ticks = order->second.price_ticks;
  event.qty = 0;

  orders_.erase(order);
  return true;
}

bool ItchParser::parseReplace(std::span<const std::byte> message,
                              MdEvent &event, uint64_t sequence) {
  if (!hasSize(message, 35)) {
    return fail("truncated order replace message");
  }

  const uint64_t old_order_id = loadU64(message, 11);
  auto order = orders_.find(old_order_id);
  if (order == orders_.end()) {
    return skip();
  }

  const uint64_t new_order_id = loadU64(message, 19);
  const uint32_t qty = loadU32(message, 27);
  const int64_t price_ticks = static_cast<int64_t>(loadU32(message, 31));
  const OrderState old_state = order->second;

  fillBaseEvent(event, sequence, timestamp(message), old_state.symbol_id);
  event.type = MdEventType::Replace;
  event.side = old_state.side;
  event.order_id = old_order_id;
  event.new_order_id = new_order_id;
  event.price_ticks = price_ticks;
  event.qty = qty;

  orders_.erase(order);
  orders_[new_order_id] =
      OrderState{old_state.symbol_id, old_state.side, price_ticks, qty};
  return true;
}

bool ItchParser::parseBrokenTrade(std::span<const std::byte> message,
                                  MdEvent &event, uint64_t sequence) {
  if (!hasSize(message, 19)) {
    return fail("truncated broken trade message");
  }

  const uint64_t match_number = loadU64(message, 11);
  auto execution = executions_by_match_.find(match_number);
  if (execution == executions_by_match_.end()) {
    return skip();
  }

  fillBaseEvent(event, sequence, timestamp(message),
                execution->second.symbol_id);
  event.side = execution->second.side;
  event.order_id = execution->second.order_id;
  event.price_ticks = execution->second.price_ticks;

  auto order = orders_.find(execution->second.order_id);
  if (order == orders_.end()) {
    event.type = MdEventType::Add;
    event.qty = execution->second.qty;
    orders_[execution->second.order_id] =
        OrderState{execution->second.symbol_id, execution->second.side,
                   execution->second.price_ticks, execution->second.qty};
  } else {
    event.type = MdEventType::Modify;
    order->second.qty += execution->second.qty;
    event.qty = order->second.qty;
  }

  executions_by_match_.erase(execution);
  return true;
}

bool ItchParser::parseStockDirectory(std::span<const std::byte> message) {
  if (!hasSize(message, 19)) {
    return fail("truncated stock directory message");
  }

  const std::string_view symbol = fixedText(message, 11, kStockSize);
  if (!symbol.empty()) {
    symbols_.intern(symbol);
  }
  return skip();
}

void ItchParser::fillBaseEvent(MdEvent &event, uint64_t sequence,
                               uint64_t timestamp_ns,
                               uint32_t symbol_id) const {
  event = MdEvent{};
  event.channel_id = channel_id_;
  event.channel_seq_no = sequence;
  event.symbol_seq_no = sequence;
  event.ts_event_ns = timestamp_ns;
  event.symbol_id = symbol_id;
  event.price_ticks = -1;
}

bool ItchParser::skip() {
  last_message_skipped_ = true;
  return false;
}

bool ItchParser::fail(std::string error) {
  last_error_ = std::move(error);
  return false;
}
