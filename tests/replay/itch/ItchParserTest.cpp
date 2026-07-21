#include "astra/parser/ItchParser.hpp"
#include "astra/book/BookManager.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;
constexpr size_t kTestOrderCapacity = 1024;
constexpr PriceLevelArenaConfig kTestPriceLevelConfig{256, 128, 128};

void appendU8(Bytes &b, uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
void appendU16(Bytes &b, uint16_t v) {
  appendU8(b, v >> 8);
  appendU8(b, v & 0xFF);
}
void appendU32(Bytes &b, uint32_t v) {
  appendU8(b, v >> 24);
  appendU8(b, (v >> 16) & 0xFF);
  appendU8(b, (v >> 8) & 0xFF);
  appendU8(b, v & 0xFF);
}
void appendU48(Bytes &b, uint64_t v) {
  for (int s = 40; s >= 0; s -= 8)
    appendU8(b, (v >> s) & 0xFF);
}
void appendU64(Bytes &b, uint64_t v) {
  for (int s = 56; s >= 0; s -= 8)
    appendU8(b, (v >> s) & 0xFF);
}
void appendStock(Bytes &b, std::string_view sym) {
  for (std::size_t i = 0; i < 8; ++i)
    appendU8(b, i < sym.size() ? static_cast<uint8_t>(sym[i]) : ' ');
}

// Build the common 11-byte ITCH prefix: type + stock_locate + tracking +
// timestamp
Bytes commonMsg(char type, uint16_t locate = 1) {
  Bytes b;
  appendU8(b, static_cast<uint8_t>(type));
  appendU16(b, locate);
  appendU16(b, 7); // tracking number (ignored)
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

Bytes msgStockDirectory(std::string_view sym, uint16_t locate = 1,
                        uint32_t round_lot_size = 100,
                        char financial_status = 'N') {
  Bytes b = commonMsg('R', locate);
  appendStock(b, sym);
  appendU8(b, 'Q'); // market category
  appendU8(b, static_cast<uint8_t>(financial_status));
  appendU32(b, round_lot_size);
  appendU8(b, 'N'); // round lots only
  appendU8(b, 'C'); // issue classification
  appendU8(b, ' ');
  appendU8(b, ' ');
  appendU8(b, 'P'); // authenticity
  while (b.size() < 39)
    appendU8(b, ' ');
  return b;
}

Bytes msgSystemEvent(char event_code) {
  Bytes b = commonMsg('S', /*locate=*/0);
  appendU8(b, static_cast<uint8_t>(event_code));
  return b;
}

auto asSpan(const Bytes &b) { return std::span<const std::byte>(b); }

struct Fixture {
  astra::symbol::StockDirectory symbols;
  BookManager books{kTestOrderCapacity, kTestPriceLevelConfig};
  ItchParser parser{symbols, books, /*channel_id=*/2};

  Fixture() {
    for (const uint16_t locate : {uint16_t{1}, uint16_t{2}, uint16_t{7},
                                  uint16_t{8}}) {
      books.setBookOrderCapacity(locate, kTestOrderCapacity);
    }
  }

  void send(const Bytes &b) { parser.handleMessage(asSpan(b)); }

  void sendAdd(uint64_t oid, char side, uint32_t qty, std::string_view sym,
               uint32_t price, uint16_t locate = 1) {
    if (!symbols.isRegistered(locate)) {
      send(msgStockDirectory(sym, locate));
    }
    if (!parser.bookUniverseReady()) {
      send(msgSystemEvent('S'));
    }
    send(msgAdd(oid, side, qty, sym, price, locate));
  }

  TopOfBook top(uint16_t locate = 1) const {
    const OrderBook *book = books.getOrderBook(locate);
    return book ? book->getTopOfBook() : TopOfBook{};
  }
};

} // namespace

TEST(ItchParserTest, AddOrderUpdatesBook) {
  Fixture f;
  f.sendAdd(555, 'B', 100, "AAPL", 1905900);

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 1905900u);
  EXPECT_EQ(t.bid_qty, 100u);
  EXPECT_EQ(t.ask_price, 0u);

  // Ticker should be registered under locate 1
  EXPECT_TRUE(f.symbols.isRegistered(1));
  EXPECT_STREQ(f.symbols.ticker(1), "AAPL");
}

TEST(ItchParserTest, ExecutionReducesOrderQuantity) {
  Fixture f;
  f.sendAdd(555, 'S', 100, "MSFT", 4102500);
  f.send(msgExecute(555, 40));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 4102500u);
  EXPECT_EQ(t.ask_qty, 60u);
}

TEST(ItchParserTest, FullExecutionRemovesOrder) {
  Fixture f;
  f.sendAdd(555, 'S', 100, "MSFT", 4102500);
  f.send(msgExecute(555, 100));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 0u);
  EXPECT_EQ(t.ask_qty, 0u);
}

TEST(ItchParserTest, CancelSharesReducesQuantity) {
  Fixture f;
  f.sendAdd(555, 'B', 100, "NVDA", 9231250);
  f.send(msgCancel(555, 25));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 9231250u);
  EXPECT_EQ(t.bid_qty, 75u);
}

TEST(ItchParserTest, CancelAllSharesRemovesOrder) {
  Fixture f;
  f.sendAdd(555, 'B', 100, "NVDA", 9231250);
  f.send(msgCancel(555, 100));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 0u);
  EXPECT_EQ(t.bid_qty, 0u);
}

TEST(ItchParserTest, DeleteRemovesOrder) {
  Fixture f;
  f.sendAdd(555, 'B', 100, "TSLA", 2510000);
  f.send(msgDelete(555));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 0u);
  EXPECT_EQ(t.bid_qty, 0u);
}

TEST(ItchParserTest, ReplaceMovesToNewPriceAndQty) {
  Fixture f;
  f.sendAdd(555, 'S', 100, "AMZN", 1873400);
  f.send(msgReplace(555, 556, 25, 1873600));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 1873600u);
  EXPECT_EQ(t.ask_qty, 25u);

  // New order id (556) should be executable
  f.send(msgExecute(556, 25));
  EXPECT_EQ(f.top().ask_price, 0u);
}

TEST(ItchParserTest, BrokenTradeIsSkippedWithoutMatchHistory) {
  Fixture f;
  f.sendAdd(555, 'B', 100, "AAPL", 1905900);
  f.send(msgExecute(555, 40, 9001));
  f.send(msgBrokenTrade(9001));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 1905900u);
  EXPECT_EQ(t.bid_qty, 60u);
  EXPECT_TRUE(f.parser.lastMessageSkipped());
}

TEST(ItchParserTest, BrokenTradeDoesNotRecreateFullyExecutedOrder) {
  Fixture f;
  f.sendAdd(555, 'S', 100, "AAPL", 1905900);
  f.send(msgExecute(555, 100, 9001));
  f.send(msgBrokenTrade(9001));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 0u);
  EXPECT_EQ(t.ask_qty, 0u);
  EXPECT_TRUE(f.parser.lastMessageSkipped());
}

TEST(ItchParserTest, UnknownExecutionIsSkipped) {
  Fixture f;
  f.send(msgStockDirectory("TEST", /*locate=*/1));
  f.send(msgSystemEvent('S'));
  f.send(msgExecute(999, 10)); // no prior add — should not crash
  EXPECT_TRUE(f.parser.lastMessageSkipped());
  EXPECT_EQ(f.top().ask_price, 0u);
}

TEST(ItchParserTest, AddForUnregisteredLocateIsFatal) {
  Fixture f;
  f.send(msgStockDirectory("TEST", /*locate=*/1));
  f.send(msgSystemEvent('S'));

  f.send(msgAdd(555, 'B', 100, "AAPL", 1905900, /*locate=*/9));

  EXPECT_FALSE(f.parser.lastError().empty());
  EXPECT_EQ(f.books.getOrderBook(9), nullptr);
}

TEST(ItchParserTest, MalformedStockDirectoryIsFatal) {
  Fixture f;
  Bytes truncated;
  appendU8(truncated, 'R');

  f.send(truncated);

  EXPECT_FALSE(f.parser.lastError().empty());
  EXPECT_EQ(f.symbols.size(), 0u);
}

TEST(ItchParserTest, StockLocateRoutesToCorrectBook) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/1));
  f.send(msgStockDirectory("MSFT", /*locate=*/2));
  f.send(msgSystemEvent('S'));
  f.send(msgAdd(1, 'B', 100, "AAPL", 500, /*locate=*/1));
  f.send(msgAdd(2, 'S', 50, "MSFT", 400, /*locate=*/2));

  EXPECT_EQ(f.top(1).bid_price, 500u);
  EXPECT_EQ(f.top(2).ask_price, 400u);
  EXPECT_EQ(f.top(1).ask_price, 0u);
  EXPECT_EQ(f.top(2).bid_price, 0u);
}

TEST(ItchParserTest, StockDirectoryDoesNotCreateBookBeforeSystemHours) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/7));

  EXPECT_EQ(f.books.getOrderBook(7), nullptr);
  EXPECT_TRUE(f.symbols.isRegistered(7));
  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
}

TEST(ItchParserTest, BookMessageBeforeReadinessIsFatalAndDoesNotAllocate) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/7));

  f.send(msgAdd(1, 'B', 100, "AAPL", 500, /*locate=*/7));

  EXPECT_FALSE(f.parser.lastError().empty());
  EXPECT_FALSE(f.parser.bookUniverseReady());
  EXPECT_EQ(f.books.getOrderBook(7), nullptr);
}

TEST(ItchParserTest, StockDirectoryAppliesProductionTickerTierPolicy) {
  astra::symbol::StockDirectory symbols;
  BookManager books{kTestOrderCapacity, kTestPriceLevelConfig};
  ItchParser parser{symbols, books};

  parser.handleMessage(asSpan(msgStockDirectory("NVDA", /*locate=*/7)));

  EXPECT_EQ(books.getOrderBook(7), nullptr);
  EXPECT_EQ(books.orderCapacityForLocate(7),
            astra::book_capacity::kUltraHotOrderCapacity);
}

TEST(ItchParserTest, SystemEventAdvancesChannelPhase) {
  Fixture f;
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::WaitingStartOfMessages);

  f.send(msgSystemEvent('O'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::StartupDirectorySpin);
  f.send(msgStockDirectory("TEST", /*locate=*/7));

  f.send(msgSystemEvent('S'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);
  EXPECT_TRUE(f.parser.bookUniverseReady());

  f.send(msgSystemEvent('Q'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);
}

TEST(ItchParserTest, EmptyDirectoryCannotBecomeReady) {
  Fixture f;
  f.send(msgSystemEvent('O'));

  f.send(msgSystemEvent('S'));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);
  EXPECT_FALSE(f.parser.bookUniverseReady());
  EXPECT_FALSE(f.parser.lastError().empty());
}

TEST(ItchParserTest, SystemHoursStartCreatesDirectoryBooks) {
  Fixture f;
  f.send(msgSystemEvent('O'));
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  EXPECT_EQ(f.books.getOrderBook(7), nullptr);

  f.send(msgSystemEvent('S'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);

  const OrderBook *book = f.books.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_TRUE(f.parser.bookUniverseReady());
}

TEST(ItchParserTest, RepeatedStockDirectoryAfterSystemHoursIsIdempotent) {
  Fixture f;
  const Bytes directory = msgStockDirectory("AAPL", /*locate=*/7);
  f.send(msgSystemEvent('O'));
  f.send(directory);
  f.send(msgSystemEvent('S'));

  const OrderBook *prepared = f.books.getOrderBook(7);
  ASSERT_NE(prepared, nullptr);
  ASSERT_TRUE(f.parser.bookUniverseReady());
  const BookManagerStats before = f.books.stats();

  f.send(directory);

  const BookManagerStats after = f.books.stats();
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_TRUE(f.parser.bookUniverseReady());
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);
  EXPECT_EQ(f.books.getOrderBook(7), prepared);
  EXPECT_EQ(f.symbols.size(), 1u);
  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
  EXPECT_EQ(after.books, before.books);
  EXPECT_EQ(after.orders.capacity, before.orders.capacity);
  EXPECT_EQ(after.price_levels.internal_nodes_in_use,
            before.price_levels.internal_nodes_in_use);
  EXPECT_EQ(after.price_levels.leaves_in_use,
            before.price_levels.leaves_in_use);
  EXPECT_EQ(after.price_levels.levels_in_use,
            before.price_levels.levels_in_use);
  EXPECT_EQ(after.late_book_creation_attempts,
            before.late_book_creation_attempts);

  f.send(msgAdd(1, 'B', 100, "AAPL", 500, /*locate=*/7));
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.top(7).bid_price, 500u);
}

TEST(ItchParserTest, RepeatedStockDirectoryRefreshesMetadataInPlace) {
  Fixture f;
  f.send(msgSystemEvent('O'));
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  f.send(msgSystemEvent('S'));

  const OrderBook *prepared = f.books.getOrderBook(7);
  ASSERT_NE(prepared, nullptr);

  f.send(msgStockDirectory("AAPL", /*locate=*/7,
                           /*round_lot_size=*/50,
                           /*financial_status=*/'D'));

  const astra::symbol::StockDirectoryEntry *entry = f.symbols.get(7);
  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_TRUE(f.parser.bookUniverseReady());
  EXPECT_EQ(f.books.getOrderBook(7), prepared);
  EXPECT_EQ(entry->round_lot_size, 50u);
  EXPECT_EQ(entry->fin_status, 'D');
  EXPECT_EQ(f.books.stats().late_book_creation_attempts, 0u);
}

TEST(ItchParserTest, StockDirectoryAfterReadinessInvalidatesReadiness) {
  Fixture f;
  f.send(msgSystemEvent('O'));
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  f.send(msgSystemEvent('S'));
  ASSERT_NE(f.books.getOrderBook(7), nullptr);

  f.send(msgStockDirectory("MSFT", /*locate=*/8));
  EXPECT_EQ(f.books.getOrderBook(8), nullptr);
  EXPECT_FALSE(f.symbols.isRegistered(8));
  EXPECT_EQ(f.symbols.ticker(8), nullptr);
  EXPECT_EQ(f.books.stats().late_book_creation_attempts, 0u);
  EXPECT_FALSE(f.parser.bookUniverseReady());
  EXPECT_FALSE(f.parser.lastError().empty());

  f.send(msgSystemEvent('Q'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);
  EXPECT_FALSE(f.parser.lastError().empty());
}

TEST(ItchParserTest, NewStockDirectoryDuringMarketHoursIsRejected) {
  Fixture f;
  f.send(msgSystemEvent('O'));
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  f.send(msgSystemEvent('S'));
  f.send(msgSystemEvent('Q'));
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);

  f.send(msgStockDirectory("MSFT", /*locate=*/8));

  EXPECT_EQ(f.books.getOrderBook(8), nullptr);
  EXPECT_EQ(f.books.stats().late_book_creation_attempts, 0u);
  EXPECT_FALSE(f.parser.bookUniverseReady());
  EXPECT_FALSE(f.parser.lastError().empty());
}

TEST(ItchParserTest, StockDirectoryCannotOverwritePreparedLocate) {
  Fixture f;
  f.send(msgSystemEvent('O'));
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  f.send(msgSystemEvent('S'));
  ASSERT_STREQ(f.symbols.ticker(7), "AAPL");

  f.send(msgStockDirectory("MSFT", /*locate=*/7));

  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
  EXPECT_FALSE(f.parser.bookUniverseReady());
  EXPECT_FALSE(f.parser.lastError().empty());
}

TEST(ItchParserTest, ResetClearsState) {
  Fixture f;
  f.sendAdd(555, 'B', 100, "AAPL", 1905900);
  f.parser.reset();
  // Parser reset does not own order-book state; execution still routes there.
  f.send(msgSystemEvent('S'));
  f.send(msgExecute(555, 100, 9001));
  EXPECT_FALSE(f.parser.lastMessageSkipped());
  f.send(msgBrokenTrade(9001));
  EXPECT_TRUE(f.parser.lastMessageSkipped());
}
