#include "replay/itch/ItchParser.hpp"
#include "replay/SymbolTable.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;

void appendU8(Bytes &bytes, uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void appendU16(Bytes &bytes, uint16_t value) {
  appendU8(bytes, static_cast<uint8_t>(value >> 8));
  appendU8(bytes, static_cast<uint8_t>(value));
}

void appendU32(Bytes &bytes, uint32_t value) {
  appendU8(bytes, static_cast<uint8_t>(value >> 24));
  appendU8(bytes, static_cast<uint8_t>(value >> 16));
  appendU8(bytes, static_cast<uint8_t>(value >> 8));
  appendU8(bytes, static_cast<uint8_t>(value));
}

void appendU48(Bytes &bytes, uint64_t value) {
  for (int shift = 40; shift >= 0; shift -= 8) {
    appendU8(bytes, static_cast<uint8_t>(value >> shift));
  }
}

void appendU64(Bytes &bytes, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    appendU8(bytes, static_cast<uint8_t>(value >> shift));
  }
}

void appendStock(Bytes &bytes, std::string_view symbol) {
  for (std::size_t i = 0; i < 8; ++i) {
    const char ch = i < symbol.size() ? symbol[i] : ' ';
    appendU8(bytes, static_cast<uint8_t>(ch));
  }
}

Bytes commonMessage(char type, uint64_t timestamp_ns = 34200123456789ULL) {
  Bytes bytes;
  appendU8(bytes, static_cast<uint8_t>(type));
  appendU16(bytes, 1);
  appendU16(bytes, 7);
  appendU48(bytes, timestamp_ns);
  return bytes;
}

Bytes addOrder(uint64_t order_id, char side, uint32_t qty,
               std::string_view symbol, uint32_t price) {
  Bytes bytes = commonMessage('A');
  appendU64(bytes, order_id);
  appendU8(bytes, static_cast<uint8_t>(side));
  appendU32(bytes, qty);
  appendStock(bytes, symbol);
  appendU32(bytes, price);
  return bytes;
}

Bytes executeOrder(uint64_t order_id, uint32_t qty) {
  Bytes bytes = commonMessage('E');
  appendU64(bytes, order_id);
  appendU32(bytes, qty);
  appendU64(bytes, 9001);
  return bytes;
}

Bytes brokenTrade(uint64_t match_number = 9001) {
  Bytes bytes = commonMessage('B');
  appendU64(bytes, match_number);
  return bytes;
}

Bytes cancelOrder(uint64_t order_id, uint32_t qty) {
  Bytes bytes = commonMessage('X');
  appendU64(bytes, order_id);
  appendU32(bytes, qty);
  return bytes;
}

Bytes deleteOrder(uint64_t order_id) {
  Bytes bytes = commonMessage('D');
  appendU64(bytes, order_id);
  return bytes;
}

Bytes replaceOrder(uint64_t old_order_id, uint64_t new_order_id, uint32_t qty,
                   uint32_t price) {
  Bytes bytes = commonMessage('U');
  appendU64(bytes, old_order_id);
  appendU64(bytes, new_order_id);
  appendU32(bytes, qty);
  appendU32(bytes, price);
  return bytes;
}

} // namespace

TEST(ItchParserTest, ParsesAddOrder) {
  SymbolTable symbols;
  ItchParser parser(symbols, 2);
  MdEvent event{};

  const Bytes message = addOrder(555, 'B', 100, "AAPL", 1905900);

  ASSERT_TRUE(parser.parseMessage(message, event));

  EXPECT_EQ(event.type, MdEventType::Add);
  EXPECT_EQ(event.channel_id, 2u);
  EXPECT_EQ(event.channel_seq_no, 1u);
  EXPECT_EQ(event.symbol_seq_no, 1u);
  EXPECT_EQ(event.symbol_id, 1u);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.price_ticks, 1905900);
  EXPECT_EQ(event.qty, 100u);
  EXPECT_EQ(event.side, 'B');
  EXPECT_EQ(event.ts_event_ns, 34200123456789ULL);
  EXPECT_EQ(parser.openOrderCount(), 1u);
  ASSERT_NE(symbols.symbol(event.symbol_id), nullptr);
  EXPECT_EQ(*symbols.symbol(event.symbol_id), "AAPL");
}

TEST(ItchParserTest, ExecutionUsesCachedOrderState) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseMessage(addOrder(555, 'S', 100, "MSFT", 4102500),
                                  event));
  ASSERT_TRUE(parser.parseMessage(executeOrder(555, 40), event));

  EXPECT_EQ(event.type, MdEventType::Execution);
  EXPECT_EQ(event.symbol_id, 1u);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.price_ticks, 4102500);
  EXPECT_EQ(event.qty, 40u);
  EXPECT_EQ(event.side, 'S');
  EXPECT_EQ(parser.openOrderCount(), 1u);

  ASSERT_TRUE(parser.parseMessage(executeOrder(555, 60), event));
  EXPECT_EQ(parser.openOrderCount(), 0u);
}

TEST(ItchParserTest, CancelBecomesModifyThenDelete) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseMessage(addOrder(555, 'B', 100, "NVDA", 9231250),
                                  event));

  ASSERT_TRUE(parser.parseMessage(cancelOrder(555, 25), event));
  EXPECT_EQ(event.type, MdEventType::Modify);
  EXPECT_EQ(event.qty, 75u);
  EXPECT_EQ(event.price_ticks, 9231250);
  EXPECT_EQ(event.side, 'B');
  EXPECT_EQ(parser.openOrderCount(), 1u);

  ASSERT_TRUE(parser.parseMessage(cancelOrder(555, 75), event));
  EXPECT_EQ(event.type, MdEventType::Delete);
  EXPECT_EQ(event.qty, 0u);
  EXPECT_EQ(parser.openOrderCount(), 0u);
}

TEST(ItchParserTest, DeleteRemovesCachedOrder) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseMessage(addOrder(555, 'B', 100, "TSLA", 2510000),
                                  event));
  ASSERT_TRUE(parser.parseMessage(deleteOrder(555), event));

  EXPECT_EQ(event.type, MdEventType::Delete);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.symbol_id, 1u);
  EXPECT_EQ(parser.openOrderCount(), 0u);
}

TEST(ItchParserTest, ReplaceMovesStateToNewOrderReference) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseMessage(addOrder(555, 'S', 100, "AMZN", 1873400),
                                  event));
  ASSERT_TRUE(parser.parseMessage(replaceOrder(555, 556, 25, 1873600), event));

  EXPECT_EQ(event.type, MdEventType::Replace);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.new_order_id, 556u);
  EXPECT_EQ(event.qty, 25u);
  EXPECT_EQ(event.price_ticks, 1873600);
  EXPECT_EQ(event.side, 'S');
  EXPECT_EQ(parser.openOrderCount(), 1u);

  ASSERT_TRUE(parser.parseMessage(executeOrder(556, 25), event));
  EXPECT_EQ(event.type, MdEventType::Execution);
  EXPECT_EQ(event.order_id, 556u);
  EXPECT_EQ(event.symbol_id, 1u);
  EXPECT_EQ(parser.openOrderCount(), 0u);
}

TEST(ItchParserTest, BrokenTradeRestoresPartialExecutionQuantity) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseMessage(addOrder(555, 'B', 100, "AAPL", 1905900),
                                  event));
  ASSERT_TRUE(parser.parseMessage(executeOrder(555, 40), event));
  ASSERT_TRUE(parser.parseMessage(brokenTrade(), event));

  EXPECT_EQ(event.type, MdEventType::Modify);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.qty, 100u);
  EXPECT_EQ(event.price_ticks, 1905900);
  EXPECT_EQ(event.side, 'B');
  EXPECT_EQ(parser.openOrderCount(), 1u);
}

TEST(ItchParserTest, BrokenTradeRestoresFullyExecutedOrder) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseMessage(addOrder(555, 'S', 100, "AAPL", 1905900),
                                  event));
  ASSERT_TRUE(parser.parseMessage(executeOrder(555, 100), event));
  ASSERT_TRUE(parser.parseMessage(brokenTrade(), event));

  EXPECT_EQ(event.type, MdEventType::Add);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.qty, 100u);
  EXPECT_EQ(event.price_ticks, 1905900);
  EXPECT_EQ(event.side, 'S');
  EXPECT_EQ(parser.openOrderCount(), 1u);
}

TEST(ItchParserTest, SkipsUnknownOrderExecutions) {
  SymbolTable symbols;
  ItchParser parser(symbols);
  MdEvent event{};

  EXPECT_FALSE(parser.parseMessage(executeOrder(999, 10), event));
  EXPECT_TRUE(parser.lastMessageSkipped());
  EXPECT_TRUE(parser.lastError().empty());
}
