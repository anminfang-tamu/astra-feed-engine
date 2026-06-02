#include "astra/book/BookManager.hpp"
#include "astra/book/TopOfBook.hpp"
#include "replay/itch/ItchParser.hpp"
#include "astra/protocol/SymbolTable.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;

void appendU8(Bytes &b, uint8_t v)  { b.push_back(static_cast<std::byte>(v)); }
void appendU16(Bytes &b, uint16_t v) { appendU8(b, v >> 8); appendU8(b, v & 0xFF); }
void appendU32(Bytes &b, uint32_t v) {
  appendU8(b, v >> 24); appendU8(b, (v >> 16) & 0xFF);
  appendU8(b, (v >>  8) & 0xFF); appendU8(b, v & 0xFF);
}
void appendU48(Bytes &b, uint64_t v) {
  for (int s = 40; s >= 0; s -= 8) appendU8(b, (v >> s) & 0xFF);
}
void appendU64(Bytes &b, uint64_t v) {
  for (int s = 56; s >= 0; s -= 8) appendU8(b, (v >> s) & 0xFF);
}
void appendStock(Bytes &b, std::string_view sym) {
  for (std::size_t i = 0; i < 8; ++i)
    appendU8(b, i < sym.size() ? static_cast<uint8_t>(sym[i]) : ' ');
}

// Build the common 11-byte ITCH prefix: type + stock_locate + tracking + timestamp
Bytes commonMsg(char type, uint16_t locate = 1) {
  Bytes b;
  appendU8(b, static_cast<uint8_t>(type));
  appendU16(b, locate);
  appendU16(b, 7);           // tracking number (ignored)
  appendU48(b, 34200123456789ULL);
  return b;
}

Bytes msgAdd(uint64_t oid, char side, uint32_t qty, std::string_view sym,
             uint32_t price, uint16_t locate = 1) {
  Bytes b = commonMsg('A', locate);
  appendU64(b, oid);
  appendU8(b, static_cast<uint8_t>(side));
  appendU32(b, qty);
  appendStock(b, sym);
  appendU32(b, price);
  return b;
}

Bytes msgExecute(uint64_t oid, uint32_t qty, uint64_t match = 9001,
                 uint16_t locate = 1) {
  Bytes b = commonMsg('E', locate);
  appendU64(b, oid);
  appendU32(b, qty);
  appendU64(b, match);
  return b;
}

Bytes msgCancel(uint64_t oid, uint32_t qty, uint16_t locate = 1) {
  Bytes b = commonMsg('X', locate);
  appendU64(b, oid);
  appendU32(b, qty);
  return b;
}

Bytes msgDelete(uint64_t oid, uint16_t locate = 1) {
  Bytes b = commonMsg('D', locate);
  appendU64(b, oid);
  return b;
}

Bytes msgReplace(uint64_t old_id, uint64_t new_id, uint32_t qty, uint32_t price,
                 uint16_t locate = 1) {
  Bytes b = commonMsg('U', locate);
  appendU64(b, old_id);
  appendU64(b, new_id);
  appendU32(b, qty);
  appendU32(b, price);
  return b;
}

Bytes msgBrokenTrade(uint64_t match = 9001, uint16_t locate = 1) {
  Bytes b = commonMsg('B', locate);
  appendU64(b, match);
  return b;
}

Bytes msgStockDirectory(std::string_view sym, uint16_t locate = 1) {
  Bytes b = commonMsg('R', locate);
  appendStock(b, sym);
  appendU8(b, 'Q'); // market category
  appendU8(b, 'N'); // financial status
  appendU32(b, 100);
  appendU8(b, 'N'); // round lots only
  appendU8(b, 'C'); // issue classification
  appendU8(b, ' ');
  appendU8(b, ' ');
  appendU8(b, 'P'); // authenticity
  while (b.size() < 39)
    appendU8(b, ' ');
  return b;
}

auto asSpan(const Bytes &b) { return std::span<const std::byte>(b); }

struct Fixture {
  SymbolTable  symbols;
  BookManager  books;
  ItchParser   parser{symbols, books, /*channel_id=*/2};

  void send(const Bytes &b) { parser.handleMessage(asSpan(b)); }

  TopOfBook top(uint16_t locate = 1) const {
    const OrderBook *book = books.getOrderBook(locate);
    return book ? book->getTopOfBook() : TopOfBook{};
  }
};

} // namespace

TEST(ItchParserTest, AddOrderUpdatesBook) {
  Fixture f;
  f.send(msgAdd(555, 'B', 100, "AAPL", 1905900));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 1905900u);
  EXPECT_EQ(t.bid_qty,   100u);
  EXPECT_EQ(t.ask_price, 0u);

  // Ticker should be registered under locate 1
  EXPECT_TRUE(f.symbols.isRegistered(1));
  EXPECT_STREQ(f.symbols.ticker(1), "AAPL");
}

TEST(ItchParserTest, ExecutionReducesOrderQuantity) {
  Fixture f;
  f.send(msgAdd(555, 'S', 100, "MSFT", 4102500));
  f.send(msgExecute(555, 40));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 4102500u);
  EXPECT_EQ(t.ask_qty,   60u);
}

TEST(ItchParserTest, FullExecutionRemovesOrder) {
  Fixture f;
  f.send(msgAdd(555, 'S', 100, "MSFT", 4102500));
  f.send(msgExecute(555, 100));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 0u);
  EXPECT_EQ(t.ask_qty,   0u);
}

TEST(ItchParserTest, CancelSharesReducesQuantity) {
  Fixture f;
  f.send(msgAdd(555, 'B', 100, "NVDA", 9231250));
  f.send(msgCancel(555, 25));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 9231250u);
  EXPECT_EQ(t.bid_qty,   75u);
}

TEST(ItchParserTest, CancelAllSharesRemovesOrder) {
  Fixture f;
  f.send(msgAdd(555, 'B', 100, "NVDA", 9231250));
  f.send(msgCancel(555, 100));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 0u);
  EXPECT_EQ(t.bid_qty,   0u);
}

TEST(ItchParserTest, DeleteRemovesOrder) {
  Fixture f;
  f.send(msgAdd(555, 'B', 100, "TSLA", 2510000));
  f.send(msgDelete(555));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 0u);
  EXPECT_EQ(t.bid_qty,   0u);
}

TEST(ItchParserTest, ReplaceMovesToNewPriceAndQty) {
  Fixture f;
  f.send(msgAdd(555, 'S', 100, "AMZN", 1873400));
  f.send(msgReplace(555, 556, 25, 1873600));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 1873600u);
  EXPECT_EQ(t.ask_qty,   25u);

  // New order id (556) should be executable
  f.send(msgExecute(556, 25));
  EXPECT_EQ(f.top().ask_price, 0u);
}

TEST(ItchParserTest, BrokenTradeRestoresPartialExecution) {
  Fixture f;
  f.send(msgAdd(555, 'B', 100, "AAPL", 1905900));
  f.send(msgExecute(555, 40, 9001));
  f.send(msgBrokenTrade(9001));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 1905900u);
  EXPECT_EQ(t.bid_qty,   100u);
}

TEST(ItchParserTest, BrokenTradeRestoresFullyExecutedOrder) {
  Fixture f;
  f.send(msgAdd(555, 'S', 100, "AAPL", 1905900));
  f.send(msgExecute(555, 100, 9001));
  f.send(msgBrokenTrade(9001));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 1905900u);
  EXPECT_EQ(t.ask_qty,   100u);
}

TEST(ItchParserTest, UnknownExecutionIsSkipped) {
  Fixture f;
  f.send(msgExecute(999, 10)); // no prior add — should not crash
  EXPECT_TRUE(f.parser.lastMessageSkipped());
  EXPECT_EQ(f.top().ask_price, 0u);
}

TEST(ItchParserTest, StockLocateRoutesToCorrectBook) {
  Fixture f;
  f.send(msgAdd(1, 'B', 100, "AAPL", 500, /*locate=*/1));
  f.send(msgAdd(2, 'S',  50, "MSFT", 400, /*locate=*/2));

  EXPECT_EQ(f.top(1).bid_price, 500u);
  EXPECT_EQ(f.top(2).ask_price, 400u);
  EXPECT_EQ(f.top(1).ask_price, 0u);
  EXPECT_EQ(f.top(2).bid_price, 0u);
}

TEST(ItchParserTest, StockDirectoryPreparesBookBeforeFirstAdd) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/7));

  const OrderBook *book = f.books.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_EQ(book->channelId(), 2u);
  EXPECT_TRUE(f.symbols.isRegistered(7));
  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
}

TEST(ItchParserTest, ResetClearsState) {
  Fixture f;
  f.send(msgAdd(555, 'B', 100, "AAPL", 1905900));
  f.parser.reset();
  // BrokenTrade for match 9001 should be a no-op after reset
  f.send(msgExecute(555, 100, 9001));
  f.send(msgBrokenTrade(9001));
  // After reset, execution state is cleared so BrokenTrade is ignored
  EXPECT_TRUE(f.parser.lastMessageSkipped());
}
