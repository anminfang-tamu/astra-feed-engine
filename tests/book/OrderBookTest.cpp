#include "astra/book/OrderBook.hpp"
#include "astra/book/TopOfBook.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace {

class DirectBook {
public:
  explicit DirectBook(uint16_t stock_locate)
      : price_levels_({256, 128, 256}),
        book_(stock_locate, 128, price_levels_) {}

  void addOrder(uint64_t order_id, uint64_t price, uint32_t qty,
                char side) noexcept {
    book_.addOrder(order_id, price, qty, side);
  }

  void cancelShares(uint64_t order_id, uint32_t canceled_qty) noexcept {
    book_.cancelShares(order_id, canceled_qty);
  }

  void deleteOrder(uint64_t order_id) noexcept {
    book_.deleteOrder(order_id);
  }

  bool trade(uint64_t order_id, uint32_t executed_qty) noexcept {
    return book_.trade(order_id, executed_qty);
  }

  void replaceOrder(uint64_t old_id, uint64_t new_id, uint64_t new_price,
                    uint32_t new_qty) noexcept {
    book_.replaceOrder(old_id, new_id, new_price, new_qty);
  }

  void reverseExecution(uint64_t order_id, uint64_t price, uint32_t qty,
                        char side) noexcept {
    book_.reverseExecution(order_id, price, qty, side);
  }

  TopOfBook getTopOfBook() const noexcept {
    return book_.getTopOfBook();
  }

  BookUpdate getBookUpdate() const noexcept {
    return book_.getBookUpdate();
  }

private:
  PriceLevelArena price_levels_;
  OrderBook book_;
};

} // namespace

TEST(OrderBookTest, StartsWithEmptyTopOfBook) {
  DirectBook book(42);
  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.symbol_id, 42u);
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
  EXPECT_EQ(top.ask_price, 0u);
  EXPECT_EQ(top.ask_qty,   0u);
  EXPECT_FALSE(top.has_bid);
  EXPECT_FALSE(top.has_ask);
}

TEST(OrderBookTest, AddOrdersUpdatesTopOfBook) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 11,  50, 'B');
  book.addOrder(3, 13,  75, 'S');
  book.addOrder(4, 12,  25, 'S');

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 11u);
  EXPECT_EQ(top.bid_qty,   50u);
  EXPECT_EQ(top.ask_price, 12u);
  EXPECT_EQ(top.ask_qty,   25u);
}

TEST(OrderBookTest, CancelSharesReducesBestLevelQuantity) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.cancelShares(1, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestBid) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 11,  50, 'B');
  book.deleteOrder(2);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
  EXPECT_EQ(top.ask_price,  0u);
  EXPECT_EQ(top.ask_qty,    0u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestAsk) {
  DirectBook book(42);
  book.addOrder(1, 12, 100, 'S');
  book.addOrder(2, 13,  50, 'S');
  book.deleteOrder(1);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price,  0u);
  EXPECT_EQ(top.bid_qty,    0u);
  EXPECT_EQ(top.ask_price, 13u);
  EXPECT_EQ(top.ask_qty,   50u);
}

TEST(OrderBookTest, ReplaceOrderMovesToNewPrice) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.replaceOrder(1, 2, 12, 75); // old id=1 → new id=2 at price 12 qty 75

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 12u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(OrderBookTest, ReplaceAtSamePriceRequeuesWithoutLosingLevel) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 10, 50, 'B');

  book.replaceOrder(1, 3, 10, 25);
  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 75u);

  book.deleteOrder(2);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_qty, 25u);
  book.deleteOrder(3);
  top = book.getTopOfBook();
  EXPECT_FALSE(top.has_bid);
}

TEST(OrderBookTest, CancelAllSharesRemovesOrder) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.cancelShares(1, 100); // fully cancel → book should be empty

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, HandlesSparseLargeOrderIdWithoutDenseAllocation) {
  constexpr uint64_t large_id = 9'000'000'000'000'000'000ULL;
  DirectBook book(42);

  book.addOrder(large_id, 10, 100, 'B');
  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);

  book.cancelShares(large_id, 60);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   40u);

  book.deleteOrder(large_id);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, BookUpdateAfterAddingOrders) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 11,  50, 'B');
  book.addOrder(3, 12,  75, 'S');
  book.addOrder(4, 13,  25, 'S');

  const BookUpdate update = book.getBookUpdate();
  EXPECT_EQ(update.symbol_id,  42u);
  EXPECT_EQ(update.bids_depth,  2u);
  EXPECT_EQ(update.asks_depth,  2u);
  EXPECT_EQ(update.bids[0].price, 11u);
  EXPECT_EQ(update.bids[0].qty,   50u);
  EXPECT_EQ(update.bids[1].price, 10u);
  EXPECT_EQ(update.bids[1].qty,  100u);
  EXPECT_EQ(update.asks[0].price, 12u);
  EXPECT_EQ(update.asks[0].qty,   75u);
  EXPECT_EQ(update.asks[1].price, 13u);
  EXPECT_EQ(update.asks[1].qty,   25u);
}

TEST(OrderBookTest, MultipleOrdersAtSamePriceAggregateQuantity) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 10,  40, 'B');

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  140u);

  book.deleteOrder(1);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(OrderBookTest, PartialTradeReducesBestLevelQuantity) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  EXPECT_TRUE(book.trade(1, 25));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(OrderBookTest, FullTradeFallsBackToNextBestBid) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 11,  50, 'B');
  EXPECT_TRUE(book.trade(2, 50));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, TradeLargerThanOrderQuantityIsIgnored) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 10, 100, 'B');
  EXPECT_FALSE(book.trade(1, 150)); // exceeds order qty — ignored

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  200u);
}

TEST(OrderBookTest, DuplicateOrderIdIsIgnored) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(1, 11,  50, 'B'); // duplicate id — ignored

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);

  book.deleteOrder(1);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, ZeroQuantityAddIsIgnored) {
  DirectBook book(42);
  book.addOrder(1, 10, 0, 'B');

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, MissingOrderOperationsDoNotChangeBook) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');

  book.cancelShares(99, 50);   // unknown id — ignored
  book.deleteOrder(99);        // unknown id — ignored
  EXPECT_FALSE(book.trade(99, 50)); // unknown id — ignored

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, BookUpdateCapsAtTopTenLevels) {
  DirectBook book(42);
  for (uint32_t i = 0; i < 12; ++i) {
    book.addOrder(100 + i, 10 + i, 10 + i, 'B');
    book.addOrder(200 + i, 20 + i, 20 + i, 'S');
  }

  const BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 10u);
  ASSERT_EQ(update.asks_depth, 10u);
  EXPECT_EQ(update.bids[0].price, 21u);
  EXPECT_EQ(update.bids[9].price, 12u);
  EXPECT_EQ(update.asks[0].price, 20u);
  EXPECT_EQ(update.asks[9].price, 29u);
}

TEST(OrderBookTest, ReverseExecutionRestoresPartialQuantity) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.trade(1, 40);
  book.reverseExecution(1, 10, 40, 'B');

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, ReverseExecutionReAddsFullyTradedOrder) {
  DirectBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.trade(1, 100); // fully consumed
  book.reverseExecution(1, 10, 100, 'B');

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, CoversFullUint32PriceDomainWithoutCenteredWindow) {
  DirectBook book(42);
  constexpr uint64_t max_price = std::numeric_limits<uint32_t>::max();

  book.addOrder(1, 0, 10, 'B');
  book.addOrder(2, max_price, 20, 'B');
  book.addOrder(3, 0, 30, 'S');
  book.addOrder(4, max_price, 40, 'S');

  TopOfBook top = book.getTopOfBook();
  EXPECT_TRUE(top.has_bid);
  EXPECT_TRUE(top.has_ask);
  EXPECT_EQ(top.bid_price, max_price);
  EXPECT_EQ(top.bid_qty, 20u);
  EXPECT_EQ(top.ask_price, 0u);
  EXPECT_EQ(top.ask_qty, 30u);

  book.deleteOrder(2);
  book.deleteOrder(3);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 10u);
  EXPECT_EQ(top.ask_price, max_price);
  EXPECT_EQ(top.ask_qty, 40u);
}

TEST(OrderBookTest, KeepsFarApartPricesDistinctAndOrdered) {
  DirectBook book(42);
  book.addOrder(1, 100, 10, 'B');
  book.addOrder(2, 10'000'000, 20, 'B');
  book.addOrder(3, 200, 30, 'S');
  book.addOrder(4, 20'000'000, 40, 'S');

  const BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 2u);
  ASSERT_EQ(update.asks_depth, 2u);
  EXPECT_EQ(update.bids[0].price, 10'000'000u);
  EXPECT_EQ(update.bids[1].price, 100u);
  EXPECT_EQ(update.asks[0].price, 200u);
  EXPECT_EQ(update.asks[1].price, 20'000'000u);
}

TEST(OrderBookTest, SameRawPriceIsIndependentOnBidAndAskSides) {
  DirectBook book(42);
  book.addOrder(1, 123'456, 10, 'B');
  book.addOrder(2, 123'456, 20, 'S');

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 123'456u);
  EXPECT_EQ(top.bid_qty, 10u);
  EXPECT_EQ(top.ask_price, 123'456u);
  EXPECT_EQ(top.ask_qty, 20u);

  book.deleteOrder(1);
  top = book.getTopOfBook();
  EXPECT_FALSE(top.has_bid);
  EXPECT_TRUE(top.has_ask);
  EXPECT_EQ(top.ask_qty, 20u);
}

TEST(OrderBookTest, AggregateQuantityUsesUint64) {
  DirectBook book(42);
  constexpr uint32_t max_qty = std::numeric_limits<uint32_t>::max();
  book.addOrder(1, 100, max_qty, 'B');
  book.addOrder(2, 100, max_qty, 'B');

  const uint64_t expected = static_cast<uint64_t>(max_qty) * 2;
  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_qty, expected);
  const BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 1u);
  EXPECT_EQ(update.bids[0].qty, expected);
  EXPECT_EQ(update.bids[0].num_orders, 2u);
}

TEST(OrderBookTest, TopTenTraversesAcrossRadixByteBoundaries) {
  DirectBook book(42);
  constexpr uint32_t prices[] = {
      0x00000000u, 0x000000ffu, 0x00000100u, 0x0000ffffu,
      0x00010000u, 0x00ffffffu, 0x01000000u, 0x7fffffffu,
      0xfffffffeu, 0xffffffffu,
  };
  for (uint32_t i = 0; i < 10; ++i) {
    book.addOrder(100 + i, prices[i], i + 1, 'B');
    book.addOrder(200 + i, prices[i], i + 11, 'S');
  }

  const BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 10u);
  ASSERT_EQ(update.asks_depth, 10u);
  for (uint32_t i = 0; i < 10; ++i) {
    EXPECT_EQ(update.bids[i].price, prices[9 - i]);
    EXPECT_EQ(update.asks[i].price, prices[i]);
  }
}
