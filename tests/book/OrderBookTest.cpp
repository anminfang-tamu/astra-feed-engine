#include "astra/book/OrderBook.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <gtest/gtest.h>

TEST(OrderBookTest, StartsWithEmptyTopOfBook) {
  OrderBook book(42);
  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.symbol_id, 42u);
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
  EXPECT_EQ(top.ask_price, 0u);
  EXPECT_EQ(top.ask_qty,   0u);
}

TEST(OrderBookTest, AddOrdersUpdatesTopOfBook) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(2, 11,  50, OrderSide::Buy);
  book.addOrder(3, 13,  75, OrderSide::Sell);
  book.addOrder(4, 12,  25, OrderSide::Sell);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 11u);
  EXPECT_EQ(top.bid_qty,   50u);
  EXPECT_EQ(top.ask_price, 12u);
  EXPECT_EQ(top.ask_qty,   25u);
}

TEST(OrderBookTest, CancelSharesReducesBestLevelQuantity) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.cancelShares(1, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestBid) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(2, 11,  50, OrderSide::Buy);
  book.deleteOrder(2);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
  EXPECT_EQ(top.ask_price,  0u);
  EXPECT_EQ(top.ask_qty,    0u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestAsk) {
  OrderBook book(42);
  book.addOrder(1, 12, 100, OrderSide::Sell);
  book.addOrder(2, 13,  50, OrderSide::Sell);
  book.deleteOrder(1);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price,  0u);
  EXPECT_EQ(top.bid_qty,    0u);
  EXPECT_EQ(top.ask_price, 13u);
  EXPECT_EQ(top.ask_qty,   50u);
}

TEST(OrderBookTest, ReplaceOrderMovesToNewPrice) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.replaceOrder(1, 2, 12, 75); // old id=1 → new id=2 at price 12 qty 75

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 12u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(OrderBookTest, CancelAllSharesRemovesOrder) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.cancelShares(1, 100); // fully cancel → book should be empty

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, HandlesSparseLargeOrderIdWithoutDenseAllocation) {
  constexpr uint64_t large_id = 9'000'000'000'000'000'000ULL;
  OrderBook book(42);

  book.addOrder(large_id, 10, 100, OrderSide::Buy);
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
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(2, 11,  50, OrderSide::Buy);
  book.addOrder(3, 12,  75, OrderSide::Sell);
  book.addOrder(4, 13,  25, OrderSide::Sell);

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
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(2, 10,  40, OrderSide::Buy);

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  140u);

  book.deleteOrder(1);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(OrderBookTest, PartialTradeReducesBestLevelQuantity) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.trade(1, 25);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(OrderBookTest, FullTradeFallsBackToNextBestBid) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(2, 11,  50, OrderSide::Buy);
  book.trade(2, 50);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, TradeLargerThanOrderQuantityIsIgnored) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(2, 10, 100, OrderSide::Buy);
  book.trade(1, 150); // exceeds order qty — ignored

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  200u);
}

TEST(OrderBookTest, DuplicateOrderIdIsIgnored) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.addOrder(1, 11,  50, OrderSide::Buy); // duplicate id — ignored

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);

  book.deleteOrder(1);
  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, ZeroQuantityAddIsIgnored) {
  OrderBook book(42);
  book.addOrder(1, 10, 0, OrderSide::Buy);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, MissingOrderOperationsDoNotChangeBook) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);

  book.cancelShares(99, 50);   // unknown id — ignored
  book.deleteOrder(99);        // unknown id — ignored
  book.trade(99, 50);          // unknown id — ignored

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, BookUpdateCapsAtTopTenLevels) {
  OrderBook book(42);
  for (uint32_t i = 0; i < 12; ++i) {
    book.addOrder(100 + i, 10 + i, 10 + i, OrderSide::Buy);
    book.addOrder(200 + i, 20 + i, 20 + i, OrderSide::Sell);
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
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.trade(1, 40);
  book.reverseExecution(1, 10, 40, OrderSide::Buy);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, ReverseExecutionReAddsFullyTradedOrder) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, OrderSide::Buy);
  book.trade(1, 100); // fully consumed
  book.reverseExecution(1, 10, 100, OrderSide::Buy);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}
