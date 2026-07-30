#include "astra/parser/ItchParser.hpp"
#include "astra/book/BookManager.hpp"
#include "astra/book/BookUpdate.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/protocol/ItchPrice.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;

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

Bytes msgAttributedAdd(uint64_t oid, char side, uint32_t qty,
                       std::string_view sym, uint32_t price,
                       uint16_t locate = 1) {
  Bytes b = msgAdd(oid, side, qty, sym, price, locate);
  b[0] = std::byte{'F'};
  appendU8(b, 'M');
  appendU8(b, 'P');
  appendU8(b, 'I');
  appendU8(b, 'D');
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

Bytes msgExecuteWithPrice(uint64_t oid, uint32_t qty, uint32_t price,
                          uint64_t match = 9001, uint16_t locate = 1) {
  Bytes b = commonMsg('C', locate);
  appendU64(b, oid);
  appendU32(b, qty);
  appendU64(b, match);
  appendU8(b, 'Y');
  appendU32(b, price);
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
                        char financial_status = 'N',
                        uint32_t round_lot_size = 100) {
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

Bytes msgSystemEvent(char event_code, uint16_t locate = 0) {
  Bytes b = commonMsg('S', locate);
  appendU8(b, static_cast<uint8_t>(event_code));
  return b;
}

Bytes msgDirectListing(std::string_view sym, uint16_t locate = 1,
                       char eligibility = 'Y') {
  Bytes b = commonMsg('O', locate);
  appendStock(b, sym);
  appendU8(b, static_cast<uint8_t>(eligibility));
  appendU32(b, 800'000);   // minimum allowable price
  appendU32(b, 1'800'000); // maximum allowable price
  appendU32(b, 1'250'000); // near execution price
  appendU64(b, 34200123456789ULL);
  appendU32(b, 1'125'000); // lower price range collar
  appendU32(b, 1'375'000); // upper price range collar
  return b;
}

Bytes msgAdministrative(char type, std::size_t length,
                        uint16_t locate = 1) {
  Bytes b = commonMsg(type, locate);
  while (b.size() < length)
    appendU8(b, ' ');
  return b;
}

Bytes withTimestamp(Bytes message, std::uint64_t timestamp) {
  for (std::size_t index = 0; index < 6; ++index) {
    const unsigned shift = static_cast<unsigned>((5 - index) * 8);
    message[5 + index] =
        static_cast<std::byte>((timestamp >> shift) & 0xffu);
  }
  return message;
}

auto asSpan(const Bytes &b) { return std::span<const std::byte>(b); }

struct Fixture {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  ItchParser parser{symbols, books, /*channel_id=*/2};
  bool system_hours_started{false};

  explicit Fixture(bool send_start_of_messages = true) {
    if (send_start_of_messages)
      send(msgSystemEvent('O'));
  }

  void send(const Bytes &b) { parser.handleMessage(asSpan(b)); }

  void add(uint64_t oid, char side, uint32_t qty, std::string_view sym,
           uint32_t price, uint16_t locate = 1) {
    if (!symbols.isRegistered(locate)) {
      send(msgStockDirectory(sym, locate));
    }
    if (!system_hours_started) {
      send(msgSystemEvent('S'));
      system_hours_started = true;
    }
    send(msgAdd(oid, side, qty, sym, price, locate));
  }

  void advanceToPostSystemHours() {
    send(msgSystemEvent('Q'));
    send(msgSystemEvent('M'));
    send(msgSystemEvent('E'));
  }

  TopOfBook top(uint16_t locate = 1) const {
    const OrderBook *book = books.getOrderBook(locate);
    return book ? book->getTopOfBook() : TopOfBook{};
  }
};

} // namespace

TEST(StockDirectoryTest, RefreshKeepsFixedReverseIndexConsistent) {
  astra::symbol::StockDirectory directory;
  const auto make_entry = [](std::string_view ticker, char status) {
    astra::symbol::StockDirectoryEntry entry{};
    for (std::size_t index = 0; index < ticker.size() && index < 8; ++index)
      entry.ticker[index] = ticker[index];
    entry.fin_status = status;
    return entry;
  };

  EXPECT_TRUE(directory.set(7, make_entry("AAPL", 'N')));
  EXPECT_TRUE(directory.set(7, make_entry("AAPL", 'D')));
  EXPECT_EQ(directory.size(), 1u);
  ASSERT_NE(directory.get(7), nullptr);
  EXPECT_EQ(directory.get(7)->fin_status, 'D');
  EXPECT_EQ(directory.locateForTicker("AAPL"), 7u);

  EXPECT_FALSE(directory.set(8, make_entry("AAPL", 'N')));
  EXPECT_FALSE(directory.isRegistered(8));
  EXPECT_EQ(directory.locateForTicker("AAPL"), 7u);

  EXPECT_FALSE(directory.set(7, make_entry("MSFT", 'N')));
  EXPECT_STREQ(directory.ticker(7), "AAPL");
  EXPECT_EQ(directory.locateForTicker("MSFT"),
            astra::symbol::kInvalidStockLocate);

  EXPECT_TRUE(directory.set(8, make_entry("MSFT", 'N')));
  EXPECT_EQ(directory.size(), 2u);
  EXPECT_EQ(directory.locateForTicker("AAPL"), 7u);
  EXPECT_EQ(directory.locateForTicker("MSFT"), 8u);
}

TEST(ItchParserTest, AddOrderUpdatesBook) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1905900);

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
  f.add(555, 'S', 100, "MSFT", 4102500);
  f.send(msgExecute(555, 40));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 4102500u);
  EXPECT_EQ(t.ask_qty, 60u);
}

TEST(ItchParserTest, FullExecutionRemovesOrder) {
  Fixture f;
  f.add(555, 'S', 100, "MSFT", 4102500);
  f.send(msgExecute(555, 100));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 0u);
  EXPECT_EQ(t.ask_qty, 0u);
}

TEST(ItchParserTest, CancelSharesReducesQuantity) {
  Fixture f;
  f.add(555, 'B', 100, "NVDA", 9231250);
  f.send(msgCancel(555, 25));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 9231250u);
  EXPECT_EQ(t.bid_qty, 75u);
}

TEST(ItchParserTest, FullCancelIsRejectedAndDeleteRemainsDistinct) {
  Fixture f;
  f.add(555, 'B', 100, "NVDA", 9231250);
  f.send(msgCancel(555, 100));

  EXPECT_EQ(f.top().bid_qty, 100u);
  EXPECT_FALSE(f.parser.lastError().empty());

  f.send(msgDelete(555));
  EXPECT_EQ(f.top().bid_qty, 0u);
}

TEST(ItchParserTest, DeleteRemovesOrder) {
  Fixture f;
  f.add(555, 'B', 100, "TSLA", 2510000);
  f.send(msgDelete(555));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 0u);
  EXPECT_EQ(t.bid_qty, 0u);
}

TEST(ItchParserTest, RejectsDayUniqueOrderReferenceReuseAfterDelete) {
  Fixture f;
  f.add(555, 'B', 100, "TSLA", 2'510'000);
  f.send(msgDelete(555));
  ASSERT_TRUE(f.parser.lastError().empty());
  ASSERT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 0u);

  f.send(msgAdd(555, 'S', 75, "TSLA", 2'511'000));

  EXPECT_EQ(f.parser.lastError(),
            "add order: duplicate order reference");
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 0u);
  EXPECT_FALSE(f.top().has_bid);
  EXPECT_FALSE(f.top().has_ask);
}

TEST(ItchParserTest, ReplaceMovesToNewPriceAndQty) {
  Fixture f;
  f.add(555, 'S', 100, "AMZN", 1873400);
  f.send(msgReplace(555, 556, 25, 1873600));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 1873600u);
  EXPECT_EQ(t.ask_qty, 25u);

  // New order id (556) should be executable
  f.send(msgExecute(556, 25));
  EXPECT_EQ(f.top().ask_price, 0u);
}

TEST(ItchParserTest, RejectsSameReferenceOrderReplaceWithoutMutation) {
  Fixture f;
  f.add(555, 'S', 100, "AMZN", 1'873'400);

  f.send(msgReplace(555, 555, 25, 1'873'600));

  EXPECT_EQ(f.parser.lastError(),
            "replacement order reference is not new");
  EXPECT_EQ(f.top().ask_price, 1'873'400u);
  EXPECT_EQ(f.top().ask_qty, 100u);
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 1u);
}

TEST(ItchParserTest,
     DirectListingPriceDiscoveryIsSupportedWithoutMutatingBookOrPhase) {
  Fixture f;
  f.add(555, 'B', 100, "DLCR", 1'200'000);
  const TopOfBook before = f.top();
  const ChannelPhase phase_before = f.parser.channelPhase();
  const Bytes direct_listing = msgDirectListing("DLCR");
  ASSERT_EQ(direct_listing.size(), 48u);

  EXPECT_FALSE(f.parser.handleMessage(asSpan(direct_listing)));

  EXPECT_TRUE(f.parser.lastMessageSkipped());
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.parser.channelPhase(), phase_before);
  const TopOfBook after = f.top();
  EXPECT_EQ(after.bid_price, before.bid_price);
  EXPECT_EQ(after.bid_qty, before.bid_qty);
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 1u);
}

TEST(ItchParserTest, DirectListingPriceDiscoveryRequiresExactWireLength) {
  Fixture f(/*send_start_of_messages=*/false);
  Bytes truncated = msgDirectListing("DLCR");
  ASSERT_EQ(truncated.size(), 48u);
  truncated.pop_back();

  EXPECT_FALSE(f.parser.handleMessage(asSpan(truncated)));

  EXPECT_EQ(f.parser.lastError(), "invalid ITCH message length");
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::WaitingStartOfMessages);
}

TEST(ItchParserTest, AcceptsMaximumNasdaqPrice4ForAddAndReplace) {
  Fixture f;
  constexpr uint32_t max_price = astra::protocol::kMaxItchPrice4;
  f.send(msgStockDirectory("MAXP"));
  f.send(msgSystemEvent('S'));

  f.send(msgAdd(1, 'B', 100, "MAXP", max_price));
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.top().bid_price, max_price);

  f.send(msgAttributedAdd(2, 'S', 200, "MAXP", max_price));
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.top().ask_price, max_price);

  f.send(msgReplace(1, 3, 75, max_price));
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.top().bid_price, max_price);
  EXPECT_EQ(f.top().bid_qty, 75u);
}

TEST(ItchParserTest,
     RejectsPricesAboveNasdaqPrice4MaximumForAddAttributedAddAndReplace) {
  Fixture f;
  constexpr uint32_t over_max = astra::protocol::kMaxItchPrice4 + 1u;
  f.send(msgStockDirectory("MAXP"));
  f.send(msgSystemEvent('S'));

  f.send(msgAdd(1, 'B', 100, "MAXP", over_max));
  EXPECT_EQ(f.parser.lastError(), "add order: price out of range");
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 0u);

  f.send(msgAttributedAdd(2, 'S', 200, "MAXP", over_max));
  EXPECT_EQ(f.parser.lastError(), "add order: price out of range");
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 0u);

  f.send(msgAdd(3, 'B', 100, "MAXP", 1'000'000));
  ASSERT_TRUE(f.parser.lastError().empty());
  f.send(msgReplace(3, 4, 75, over_max));
  EXPECT_EQ(f.parser.lastError(), "order replace: price out of range");
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 1u);
  EXPECT_EQ(f.top().bid_price, 1'000'000u);
  EXPECT_EQ(f.top().bid_qty, 100u);
}

TEST(ItchParserTest, BrokenTradeIsSkippedWithoutMatchHistory) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1905900);
  f.send(msgExecute(555, 40, 9001));
  f.send(msgBrokenTrade(9001));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.bid_price, 1905900u);
  EXPECT_EQ(t.bid_qty, 60u);
  EXPECT_TRUE(f.parser.lastMessageSkipped());
}

TEST(ItchParserTest, BrokenTradeDoesNotRecreateFullyExecutedOrder) {
  Fixture f;
  f.add(555, 'S', 100, "AAPL", 1905900);
  f.send(msgExecute(555, 100, 9001));
  f.send(msgBrokenTrade(9001));

  const TopOfBook t = f.top();
  EXPECT_EQ(t.ask_price, 0u);
  EXPECT_EQ(t.ask_qty, 0u);
  EXPECT_TRUE(f.parser.lastMessageSkipped());
}

TEST(ItchParserTest, UnknownExecutionIsSkipped) {
  Fixture f;
  f.send(msgExecute(999, 10)); // no prior add — should not crash
  EXPECT_TRUE(f.parser.lastMessageSkipped());
  EXPECT_EQ(f.top().ask_price, 0u);
}

TEST(ItchParserTest, StockLocateRoutesToCorrectBook) {
  Fixture f;
  f.add(1, 'B', 100, "AAPL", 500, /*locate=*/1);
  f.add(2, 'S', 50, "MSFT", 400, /*locate=*/2);

  EXPECT_EQ(f.top(1).bid_price, 500u);
  EXPECT_EQ(f.top(2).ask_price, 400u);
  EXPECT_EQ(f.top(1).ask_price, 0u);
  EXPECT_EQ(f.top(2).bid_price, 0u);
}

TEST(ItchParserTest,
     MultiSymbolCrudMaintainsIndependentAggregatedPriceLevels) {
  Fixture f;
  constexpr std::uint16_t aapl = 7;
  constexpr std::uint16_t msft = 8;

  f.send(msgStockDirectory("AAPL", aapl));
  f.send(msgStockDirectory("MSFT", msft));
  f.send(msgSystemEvent('S'));

  // Build two independent books, including multiple orders at one price.
  f.send(msgAdd(101, 'B', 100, "AAPL", 100'000, aapl));
  f.send(msgAttributedAdd(102, 'B', 40, "AAPL", 100'000, aapl));
  f.send(msgAdd(103, 'B', 50, "AAPL", 99'000, aapl));
  f.send(msgAdd(104, 'S', 70, "AAPL", 101'000, aapl));
  f.send(msgAdd(201, 'S', 200, "MSFT", 500'000, msft));
  f.send(msgAttributedAdd(202, 'S', 50, "MSFT", 500'000, msft));
  f.send(msgAdd(203, 'B', 80, "MSFT", 499'000, msft));

  const OrderBook *const aapl_book = f.books.getOrderBook(aapl);
  const OrderBook *const msft_book = f.books.getOrderBook(msft);
  ASSERT_NE(aapl_book, nullptr);
  ASSERT_NE(msft_book, nullptr);
  const BookUpdate aapl_initial = aapl_book->getBookUpdate();
  const BookUpdate msft_initial = msft_book->getBookUpdate();

  ASSERT_EQ(aapl_initial.bids_depth, 2u);
  EXPECT_EQ(aapl_initial.bids[0].price, 100'000u);
  EXPECT_EQ(aapl_initial.bids[0].qty, 140u);
  EXPECT_EQ(aapl_initial.bids[0].num_orders, 2u);
  EXPECT_EQ(aapl_initial.bids[1].price, 99'000u);
  EXPECT_EQ(aapl_initial.bids[1].qty, 50u);
  EXPECT_EQ(aapl_initial.asks[0].price, 101'000u);
  EXPECT_EQ(aapl_initial.asks[0].qty, 70u);

  ASSERT_EQ(msft_initial.asks_depth, 1u);
  EXPECT_EQ(msft_initial.asks[0].price, 500'000u);
  EXPECT_EQ(msft_initial.asks[0].qty, 250u);
  EXPECT_EQ(msft_initial.asks[0].num_orders, 2u);
  EXPECT_EQ(msft_initial.bids[0].price, 499'000u);
  EXPECT_EQ(msft_initial.bids[0].qty, 80u);

  // X, E, U, C, and D must update the referenced locate and price level only.
  f.send(msgCancel(101, 25, aapl));
  f.send(msgExecute(201, 200, 9'001, msft));
  f.send(msgReplace(103, 105, 60, 100'500, aapl));
  f.send(msgExecuteWithPrice(104, 20, 101'100, 9'002, aapl));
  f.send(msgDelete(202, msft));

  const BookUpdate aapl_after = aapl_book->getBookUpdate();
  const BookUpdate msft_after = msft_book->getBookUpdate();

  ASSERT_EQ(aapl_after.bids_depth, 2u);
  EXPECT_EQ(aapl_after.bids[0].price, 100'500u);
  EXPECT_EQ(aapl_after.bids[0].qty, 60u);
  EXPECT_EQ(aapl_after.bids[0].num_orders, 1u);
  EXPECT_EQ(aapl_after.bids[1].price, 100'000u);
  EXPECT_EQ(aapl_after.bids[1].qty, 115u);
  EXPECT_EQ(aapl_after.bids[1].num_orders, 2u);
  ASSERT_EQ(aapl_after.asks_depth, 1u);
  EXPECT_EQ(aapl_after.asks[0].price, 101'000u);
  EXPECT_EQ(aapl_after.asks[0].qty, 50u);
  EXPECT_EQ(aapl_after.asks[0].num_orders, 1u);

  EXPECT_EQ(msft_after.asks_depth, 0u);
  ASSERT_EQ(msft_after.bids_depth, 1u);
  EXPECT_EQ(msft_after.bids[0].price, 499'000u);
  EXPECT_EQ(msft_after.bids[0].qty, 80u);
  EXPECT_EQ(msft_after.bids[0].num_orders, 1u);
  EXPECT_EQ(aapl_book->liveOrderCount(), 4u);
  EXPECT_EQ(msft_book->liveOrderCount(), 1u);
  EXPECT_TRUE(f.parser.lastError().empty());
}

TEST(ItchParserTest, StockDirectoryDoesNotCreateBookBeforeSystemHours) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/7));

  EXPECT_EQ(f.books.getOrderBook(7), nullptr);
  EXPECT_TRUE(f.symbols.isRegistered(7));
  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
}

TEST(ItchParserTest, RejectsStockDirectoryBeforeStartOfMessagesWithoutMutation) {
  Fixture f(/*send_start_of_messages=*/false);

  f.send(msgStockDirectory("AAPL", /*locate=*/7));

  EXPECT_EQ(f.parser.lastError(),
            "first ITCH message is not System Event O");
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::WaitingStartOfMessages);
  EXPECT_FALSE(f.symbols.isRegistered(7));
  EXPECT_EQ(f.books.getOrderBook(7), nullptr);
}

TEST(ItchParserTest, RejectsSystemHoursBeforeStartOfMessagesWithoutMutation) {
  Fixture f(/*send_start_of_messages=*/false);

  f.send(msgSystemEvent('S'));

  EXPECT_EQ(f.parser.lastError(),
            "first ITCH message is not System Event O");
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::WaitingStartOfMessages);
  EXPECT_EQ(f.symbols.size(), 0u);
  EXPECT_EQ(f.books.getOrderBook(1), nullptr);
}

TEST(ItchParserTest, RejectsNonzeroSystemEventStockLocateWithoutPhaseChange) {
  Fixture f(/*send_start_of_messages=*/false);

  f.send(msgSystemEvent('O', /*locate=*/7));

  EXPECT_EQ(f.parser.lastError(),
            "System Event stock locate is not zero");
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::WaitingStartOfMessages);
  EXPECT_EQ(f.symbols.size(), 0u);
}

TEST(ItchParserTest, SystemEventAdvancesChannelPhase) {
  Fixture f(/*send_start_of_messages=*/false);
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::WaitingStartOfMessages);

  f.send(msgSystemEvent('O'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::StartupDirectorySpin);

  f.send(msgSystemEvent('S'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);

  f.send(msgSystemEvent('Q'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);

  f.send(msgSystemEvent('M'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostMarketHours);

  f.send(msgSystemEvent('E'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);

  f.send(msgSystemEvent('C'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::EndOfMessages);
}

TEST(ItchParserTest, BackwardSystemEventIsRejectedWithoutPhaseRegression) {
  Fixture f;
  f.send(msgSystemEvent('S'));
  f.send(msgSystemEvent('Q'));
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);

  f.send(msgSystemEvent('S'));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);
  EXPECT_EQ(f.parser.lastError(),
            "out-of-order or duplicate system event");
}

TEST(ItchParserTest, SkippedDailySystemEventIsRejected) {
  Fixture f;

  f.send(msgSystemEvent('Q'));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::StartupDirectorySpin);
  EXPECT_EQ(f.parser.lastError(),
            "out-of-order or duplicate system event");
}

TEST(ItchParserTest, UnknownSystemEventCodeIsRejected) {
  Fixture f;

  f.send(msgSystemEvent('Z'));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::StartupDirectorySpin);
  EXPECT_EQ(f.parser.lastError(), "unsupported system event code");
}

TEST(ItchParserTest, EmergencySystemEventsDoNotChangeDailyPhase) {
  Fixture f;
  for (const char code : {'A', 'R', 'B'}) {
    f.send(msgSystemEvent(code));
    EXPECT_TRUE(f.parser.lastError().empty());
    EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::StartupDirectorySpin);
  }
}

TEST(ItchParserTest, SystemHoursStartCreatesDirectoryBooks) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  EXPECT_EQ(f.books.getOrderBook(7), nullptr);

  f.send(msgSystemEvent('S'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::SystemHours);

  const OrderBook *book = f.books.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->liveOrderCount(), 0u);
}

TEST(ItchParserTest, StockDirectoryDuringSystemHoursCreatesBookImmediately) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", /*locate=*/7));
  f.send(msgSystemEvent('S'));
  ASSERT_NE(f.books.getOrderBook(7), nullptr);

  f.send(msgStockDirectory("MSFT", /*locate=*/8));
  ASSERT_NE(f.books.getOrderBook(8), nullptr);
  EXPECT_TRUE(f.symbols.isRegistered(8));
  EXPECT_STREQ(f.symbols.ticker(8), "MSFT");

  f.send(msgSystemEvent('Q'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);
}

TEST(ItchParserTest, OrderExecutedWithPriceUsesIndependentWireHandler) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1905900);

  f.send(msgExecuteWithPrice(555, 40, 1906000));

  EXPECT_EQ(f.top().bid_qty, 60u);
  EXPECT_TRUE(f.parser.lastError().empty());
}

TEST(ItchParserTest, InvalidOrderExecutedWithPriceDoesNotMutateBook) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1'905'900);
  const TopOfBook before = f.top();

  f.send(msgExecuteWithPrice(
      555, 40, astra::protocol::kMaxItchPrice4 + 1u));

  EXPECT_EQ(f.parser.lastError(),
            "order execution with price: price out of range");
  EXPECT_EQ(f.top().bid_price, before.bid_price);
  EXPECT_EQ(f.top().bid_qty, before.bid_qty);
  EXPECT_EQ(f.top().timestamp, before.timestamp);
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 1u);
}

TEST(ItchParserTest, RepeatedStockDirectoryAfterSsRefreshesMetadataInPlace) {
  Fixture f;
  f.send(msgStockDirectory("CFG-D", 1335, 'N', 100));
  f.send(msgSystemEvent('S'));
  const OrderBook *original_book = f.books.getOrderBook(1335);
  ASSERT_NE(original_book, nullptr);

  f.send(msgStockDirectory("CFG-D", 1335, 'D', 50));

  EXPECT_EQ(f.books.getOrderBook(1335), original_book);
  ASSERT_NE(f.symbols.get(1335), nullptr);
  EXPECT_EQ(f.symbols.get(1335)->fin_status, 'D');
  EXPECT_EQ(f.symbols.get(1335)->round_lot_size, 50u);
  EXPECT_EQ(f.symbols.size(), 1u);
  EXPECT_TRUE(f.parser.lastError().empty());
}

TEST(ItchParserTest, RepeatedLateDirectoryPreservesLiveBookState) {
  Fixture f;
  f.add(555, 'B', 100, "CFG-D", 2'500'000, 1335);
  const OrderBook *const original_book = f.books.getOrderBook(1335);
  ASSERT_NE(original_book, nullptr);

  f.send(msgStockDirectory("CFG-D", 1335, 'D', 50));

  EXPECT_EQ(f.books.getOrderBook(1335), original_book);
  EXPECT_TRUE(f.top(1335).has_bid);
  EXPECT_EQ(f.top(1335).bid_price, 2'500'000u);
  EXPECT_EQ(f.top(1335).bid_qty, 100u);
  EXPECT_TRUE(f.parser.lastError().empty());
}

TEST(ItchParserTest, NewStockDirectoryAfterSsPreparesBookImmediately) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", 7));
  f.send(msgSystemEvent('S'));
  f.send(msgSystemEvent('Q'));

  f.send(msgStockDirectory("MSFT", 8));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::MarketHours);
  ASSERT_NE(f.books.getOrderBook(8), nullptr);
  EXPECT_STREQ(f.symbols.ticker(8), "MSFT");
  EXPECT_TRUE(f.parser.lastError().empty());
}

TEST(ItchParserTest, ConflictingRepeatedStockDirectoryIsRejected) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", 7));
  f.send(msgSystemEvent('S'));
  const OrderBook *original_book = f.books.getOrderBook(7);

  f.send(msgStockDirectory("MSFT", 7));

  EXPECT_EQ(f.books.getOrderBook(7), original_book);
  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
  EXPECT_FALSE(f.parser.lastError().empty());
}

TEST(ItchParserTest,
     RejectsDuplicateTickerAtAnotherStockLocateWithoutMutation) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", 7));
  ASSERT_EQ(f.symbols.locateForTicker("AAPL"), 7u);
  f.send(msgSystemEvent('S'));
  const OrderBook *const original_book = f.books.getOrderBook(7);
  ASSERT_NE(original_book, nullptr);

  f.send(msgStockDirectory("AAPL", 8));

  EXPECT_EQ(f.parser.lastError(),
            "ticker is already registered to another stock locate");
  EXPECT_EQ(f.symbols.size(), 1u);
  EXPECT_TRUE(f.symbols.isRegistered(7));
  EXPECT_FALSE(f.symbols.isRegistered(8));
  EXPECT_STREQ(f.symbols.ticker(7), "AAPL");
  EXPECT_EQ(f.symbols.locateForTicker("AAPL"), 7u);
  EXPECT_EQ(f.books.getOrderBook(7), original_book);
  EXPECT_EQ(f.books.getOrderBook(8), nullptr);
}

TEST(ItchParserTest,
     RejectsAddStockIdentityErrorsBeforeAnyBookMutation) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", 7));
  f.send(msgSystemEvent('S'));
  const OrderBook *const book = f.books.getOrderBook(7);
  ASSERT_NE(book, nullptr);

  f.send(withTimestamp(msgAdd(1, 'B', 100, "MSFT", 100, 7), 11));
  EXPECT_EQ(f.parser.lastError(),
            "add order stock does not match registered ticker");
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_EQ(book->getTopOfBook().timestamp, 0u);

  f.send(withTimestamp(
      msgAttributedAdd(2, 'S', 100, "MSFT", 101, 7), 12));
  EXPECT_EQ(f.parser.lastError(),
            "add order stock does not match registered ticker");
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_EQ(book->getBookUpdate().timestamp, 0u);

  f.send(withTimestamp(msgAdd(3, 'B', 100, "AAPL", 100, 8), 13));
  EXPECT_EQ(f.parser.lastError(),
            "add order uses unregistered stock locate");
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_EQ(f.books.getOrderBook(8), nullptr);

  f.send(withTimestamp(
      msgAttributedAdd(4, 'S', 100, "AAPL", 101, 8), 14));
  EXPECT_EQ(f.parser.lastError(),
            "add order uses unregistered stock locate");
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_EQ(f.books.getOrderBook(8), nullptr);
}

TEST(ItchParserTest,
     SuccessfulCrudPublishesExactWireTimestampAndFailuresDoNotAdvanceIt) {
  Fixture f;
  constexpr std::uint16_t locate = 7;
  constexpr std::uint64_t max_valid_timestamp = 86'399'999'999'999ULL;
  constexpr std::uint64_t max_raw_48_bit = (std::uint64_t{1} << 48) - 1;
  f.send(msgStockDirectory("AAPL", locate));
  f.send(msgSystemEvent('S'));
  const OrderBook *const book = f.books.getOrderBook(locate);
  ASSERT_NE(book, nullptr);

  const auto expect_timestamp = [book](std::uint64_t expected) {
    EXPECT_EQ(book->getTopOfBook().timestamp, expected);
    EXPECT_EQ(book->getBookUpdate().timestamp, expected);
  };

  f.send(withTimestamp(
      msgAdd(1, 'B', 100, "AAPL", 100, locate), 1));
  ASSERT_TRUE(f.parser.lastError().empty());
  expect_timestamp(1);

  f.send(withTimestamp(
      msgAttributedAdd(2, 'S', 200, "AAPL", 200, locate), 2));
  ASSERT_TRUE(f.parser.lastError().empty());
  expect_timestamp(2);

  f.send(withTimestamp(
      msgAdd(3, 'B', 50, "MSFT", 150, locate), 99));
  EXPECT_EQ(f.parser.lastError(),
            "add order stock does not match registered ticker");
  EXPECT_EQ(book->liveOrderCount(), 2u);
  expect_timestamp(2);

  f.send(withTimestamp(msgCancel(1, 100, locate), 99));
  EXPECT_EQ(f.parser.lastError(),
            "order cancel: quantity exceeds remaining order");
  EXPECT_EQ(book->getTopOfBook().bid_qty, 100u);
  expect_timestamp(2);

  f.send(withTimestamp(msgCancel(1, 10, locate), 3));
  ASSERT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(book->getTopOfBook().bid_qty, 90u);
  expect_timestamp(3);

  f.send(withTimestamp(msgExecute(1, 10, 9'001, locate), 4));
  ASSERT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(book->getTopOfBook().bid_qty, 80u);
  expect_timestamp(4);

  f.send(withTimestamp(
      msgExecuteWithPrice(1, 10, 101, 9'002, locate), 5));
  ASSERT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(book->getTopOfBook().bid_qty, 70u);
  expect_timestamp(5);

  f.send(withTimestamp(msgReplace(1, 3, 60, 110, locate), 6));
  ASSERT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(book->getTopOfBook().bid_price, 110u);
  EXPECT_EQ(book->getTopOfBook().bid_qty, 60u);
  expect_timestamp(6);

  f.send(withTimestamp(msgDelete(3, locate), max_raw_48_bit));
  EXPECT_EQ(f.parser.lastError(),
            "ITCH timestamp is outside trading day");
  EXPECT_EQ(book->getTopOfBook().bid_qty, 60u);
  EXPECT_EQ(book->liveOrderCount(), 2u);
  expect_timestamp(6);

  f.send(withTimestamp(msgDelete(3, locate), max_valid_timestamp));
  ASSERT_TRUE(f.parser.lastError().empty());
  EXPECT_FALSE(book->getTopOfBook().has_bid);
  EXPECT_EQ(book->liveOrderCount(), 1u);
  expect_timestamp(max_valid_timestamp);

  f.send(withTimestamp(msgDelete(999, locate), 7));
  EXPECT_EQ(f.parser.lastError(), "order delete: order not found");
  EXPECT_EQ(book->liveOrderCount(), 1u);
  expect_timestamp(max_valid_timestamp);
}

TEST(ItchParserTest, DeleteRemainsActiveAfterEndOfSystemHours) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1905900);
  f.advanceToPostSystemHours();
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);

  f.send(msgDelete(555));

  EXPECT_EQ(f.top().bid_qty, 0u);
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  f.send(msgSystemEvent('C'));
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::EndOfMessages);
}

TEST(ItchParserTest, SystemEventEndOfHoursDoesNotMutateBook) {
  Fixture f;
  f.add(555, 'S', 100, "AAPL", 1'905'900);
  const TopOfBook before = f.top();

  f.advanceToPostSystemHours();

  const TopOfBook after = f.top();
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  EXPECT_EQ(after.ask_price, before.ask_price);
  EXPECT_EQ(after.ask_qty, before.ask_qty);
  EXPECT_EQ(after.has_ask, before.has_ask);
}

TEST(ItchParserTest, OrderExecutionIsRejectedAfterEndOfSystemHours) {
  Fixture f;
  f.add(555, 'S', 100, "AAPL", 1'905'900);
  f.advanceToPostSystemHours();

  f.send(msgExecute(555, 40));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  EXPECT_EQ(f.top().ask_qty, 100u);
  EXPECT_EQ(f.parser.lastError(),
            "ITCH message is not permitted after System Event E");
}

TEST(ItchParserTest,
     OrderExecutionWithPriceIsRejectedAfterEndOfSystemHours) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1'905'900);
  f.advanceToPostSystemHours();

  f.send(msgExecuteWithPrice(555, 25, 1'906'000));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  EXPECT_EQ(f.top().bid_qty, 100u);
  EXPECT_EQ(f.parser.lastError(),
            "ITCH message is not permitted after System Event E");
}

TEST(ItchParserTest,
     CancelDeleteAndBrokenTradeRemainValidAfterEndOfSystemHours) {
  Fixture f;
  f.add(1, 'B', 100, "AAPL", 100);
  f.add(2, 'S', 50, "AAPL", 200);
  f.advanceToPostSystemHours();
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);

  f.send(msgAdd(2, 'S', 80, "AAPL", 200));
  EXPECT_FALSE(f.parser.lastError().empty());
  f.send(msgAttributedAdd(3, 'S', 70, "AAPL", 300));
  EXPECT_FALSE(f.parser.lastError().empty());
  // The certified 2026 teardown contains D and X interleaved.
  f.send(msgDelete(2));
  EXPECT_TRUE(f.parser.lastError().empty());
  f.send(msgCancel(1, 10));
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.top().bid_qty, 90u);
  f.send(msgReplace(1, 4, 60, 250));
  EXPECT_FALSE(f.parser.lastError().empty());
  f.send(msgExecute(1, 40));
  EXPECT_FALSE(f.parser.lastError().empty());
  f.send(msgExecuteWithPrice(1, 25, 101));
  EXPECT_FALSE(f.parser.lastError().empty());
  EXPECT_EQ(f.top().bid_qty, 90u);

  f.send(msgBrokenTrade());
  EXPECT_TRUE(f.parser.lastError().empty());
  f.send(msgDelete(1));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  EXPECT_TRUE(f.parser.lastError().empty());
  EXPECT_EQ(f.top().bid_qty, 0u);
  EXPECT_EQ(f.top().ask_qty, 0u);
  EXPECT_EQ(f.books.getOrderBook(1)->liveOrderCount(), 0u);
}

TEST(ItchParserTest,
     DirectoryMessagesAreRejectedAfterEndOfSystemHours) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", 7, 'N', 100));
  f.send(msgSystemEvent('S'));
  f.advanceToPostSystemHours();
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  const OrderBook *const original_book = f.books.getOrderBook(7);
  ASSERT_NE(original_book, nullptr);

  f.send(msgStockDirectory("AAPL", 7, 'D', 50));
  EXPECT_EQ(f.books.getOrderBook(7), original_book);
  ASSERT_NE(f.symbols.get(7), nullptr);
  EXPECT_EQ(f.symbols.get(7)->fin_status, 'N');
  EXPECT_EQ(f.symbols.get(7)->round_lot_size, 100u);
  EXPECT_FALSE(f.parser.lastError().empty());

  f.send(msgStockDirectory("MSFT", 8));
  EXPECT_EQ(f.books.getOrderBook(8), nullptr);
  EXPECT_FALSE(f.symbols.isRegistered(8));
  EXPECT_FALSE(f.parser.lastError().empty());

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
}

TEST(ItchParserTest,
     AdministrativeAndEmergencyMessagesAreRejectedAfterEndOfSystemHours) {
  Fixture f;
  f.add(1, 'B', 100, "AAPL", 100);
  f.advanceToPostSystemHours();

  constexpr std::array<std::pair<char, std::size_t>, 13> admin_types{{
      {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12},
      {'K', 28}, {'J', 35}, {'h', 21}, {'O', 48}, {'P', 44},
      {'Q', 40}, {'I', 50}, {'N', 20},
  }};
  for (const auto &[type, length] : admin_types) {
    f.send(msgAdministrative(type, length));
    EXPECT_EQ(f.parser.lastError(),
              "ITCH message is not permitted after System Event E")
        << "type=" << type;
    EXPECT_EQ(f.top().bid_qty, 100u) << "type=" << type;
  }

  for (const char event_code : {'A', 'R', 'B'}) {
    f.send(msgSystemEvent(event_code));
    EXPECT_EQ(f.parser.lastError(),
              "ITCH message is not permitted after System Event E")
        << "event_code=" << event_code;
    EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
    EXPECT_EQ(f.top().bid_qty, 100u);
  }
}

TEST(ItchParserTest, EndOfMessagesRejectsLiveOrders) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1'905'900);
  f.advanceToPostSystemHours();
  f.send(msgSystemEvent('C'));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  EXPECT_EQ(f.top().bid_qty, 100u);
  EXPECT_EQ(f.parser.lastError(), "end of messages with live orders");
}

TEST(ItchParserTest, EndOfMessagesScansLiveOrdersAcrossAllSymbols) {
  Fixture f;
  f.add(1, 'B', 100, "AAPL", 100, 1);
  f.add(2, 'S', 75, "MSFT", 200, 2);
  f.advanceToPostSystemHours();
  f.send(msgDelete(1, 1));
  ASSERT_EQ(f.top(1).bid_qty, 0u);
  ASSERT_EQ(f.top(2).ask_qty, 75u);

  f.send(msgSystemEvent('C'));

  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostSystemHours);
  EXPECT_EQ(f.parser.lastError(), "end of messages with live orders");
  EXPECT_EQ(f.top(2).ask_qty, 75u);
}

TEST(ItchParserTest, EndOfMessagesIsTerminalAfterDeleteTail) {
  Fixture f;
  f.add(555, 'B', 100, "AAPL", 1'905'900);
  f.advanceToPostSystemHours();
  f.send(msgDelete(555));
  f.send(msgSystemEvent('C'));
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::EndOfMessages);

  f.send(msgStockDirectory("MSFT", 8));
  EXPECT_EQ(f.books.getOrderBook(8), nullptr);
  EXPECT_FALSE(f.symbols.isRegistered(8));
  EXPECT_FALSE(f.parser.lastError().empty());
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::EndOfMessages);
}

TEST(ItchParserTest, LateDirectoryAndPostMarketExecutionRemainActive) {
  Fixture f;
  f.send(msgStockDirectory("AAPL", 7));
  f.send(msgSystemEvent('S'));

  // SS freezes hot-path preparation, but it must not seal directory
  // membership. A genuinely new late R is prepared on the admin path.
  f.send(msgStockDirectory("MSFT", 8));
  ASSERT_NE(f.books.getOrderBook(8), nullptr);
  f.send(msgAdd(555, 'S', 100, "MSFT", 4102500, 8));
  ASSERT_EQ(f.top(8).ask_qty, 100u);

  f.send(msgSystemEvent('Q'));
  f.send(msgSystemEvent('M'));
  ASSERT_EQ(f.parser.channelPhase(), ChannelPhase::PostMarketHours);

  // Message type E is Order Executed. It remains a book mutation after the
  // system-event M (End of Market Hours).
  f.send(msgExecute(555, 40, 9001, 8));

  EXPECT_EQ(f.top(8).ask_qty, 60u);
  EXPECT_EQ(f.parser.channelPhase(), ChannelPhase::PostMarketHours);
  EXPECT_TRUE(f.parser.lastError().empty());
}

TEST(ItchParserTest, SupportsMaximumWireStockLocate) {
  Fixture f;
  constexpr uint16_t kMaxWireLocate = UINT16_MAX;

  f.add(555, 'B', 100, "MAXLOC", 1905900, kMaxWireLocate);

  EXPECT_TRUE(f.symbols.isRegistered(kMaxWireLocate));
  EXPECT_EQ(f.top(kMaxWireLocate).bid_qty, 100u);
}
