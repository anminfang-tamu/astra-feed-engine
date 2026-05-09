#include "astra/book/OrderBook.hpp"
#include "astra/book/TopOfBook.hpp"

#include <gtest/gtest.h>

namespace {

MarketDataMessage makeMessage(uint64_t order_id, uint32_t symbol_id,
                              uint32_t price, uint32_t qty, OrderSide side,
                              MessageType type) {
  MarketDataMessage msg{};
  msg.order_id = order_id;
  msg.symbol_id = symbol_id;
  msg.price = price;
  msg.qty = qty;
  msg.side = side;
  msg.type = type;
  return msg;
}

} // namespace

TEST(OrderBookTest, StartsWithEmptyTopOfBook) {
  OrderBook book(42);

  const TopOfBook top = book.getTopOfBook();

  EXPECT_EQ(top.symbol_id, 42u);
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
  EXPECT_EQ(top.ask_price, 0u);
  EXPECT_EQ(top.ask_qty, 0u);
}

TEST(OrderBookTest, AddOrdersUpdatesTopOfBook) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 11, 50, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(3, 42, 13, 75, OrderSide::Sell, MessageType::AddOrder));
  book.addOrder(
      makeMessage(4, 42, 12, 25, OrderSide::Sell, MessageType::AddOrder));

  const TopOfBook top = book.getTopOfBook();

  EXPECT_EQ(top.bid_price, 11u);
  EXPECT_EQ(top.bid_qty, 50u);
  EXPECT_EQ(top.ask_price, 12u);
  EXPECT_EQ(top.ask_qty, 25u);
}

TEST(OrderBookTest, ModifyOrderUpdatesBestLevelQuantity) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.modifyOrder(
      makeMessage(1, 42, 10, 40, OrderSide::Buy, MessageType::ModifyOrder));

  const TopOfBook top = book.getTopOfBook();

  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 40u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestBid) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 11, 50, OrderSide::Buy, MessageType::AddOrder));

  book.deleteOrder(
      makeMessage(2, 42, 0, 0, OrderSide::Buy, MessageType::DeleteOrder));

  const TopOfBook top = book.getTopOfBook();

  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 100u);
  EXPECT_EQ(top.ask_price, 0u);
  EXPECT_EQ(top.ask_qty, 0u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestAsk) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 12, 100, OrderSide::Sell, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 13, 50, OrderSide::Sell, MessageType::AddOrder));

  book.deleteOrder(
      makeMessage(1, 42, 0, 0, OrderSide::Sell, MessageType::DeleteOrder));

  const TopOfBook top = book.getTopOfBook();

  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
  EXPECT_EQ(top.ask_price, 13u);
  EXPECT_EQ(top.ask_qty, 50u);
}

TEST(OrderBookTest, ModifyOrderMovesBestBidToNewPrice) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.modifyOrder(
      makeMessage(1, 42, 12, 75, OrderSide::Buy, MessageType::ModifyOrder));

  const TopOfBook top = book.getTopOfBook();

  EXPECT_EQ(top.bid_price, 12u);
  EXPECT_EQ(top.bid_qty, 75u);
}

TEST(OrderBookTest, HandlesSparseLargeOrderIdWithoutDenseAllocation) {
  constexpr uint64_t large_order_id = 9'000'000'000'000'000'000ULL;
  OrderBook book(42);

  ASSERT_NO_THROW(book.addOrder(makeMessage(
      large_order_id, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder)));

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 100u);

  ASSERT_NO_THROW(book.modifyOrder(makeMessage(
      large_order_id, 42, 12, 40, OrderSide::Buy, MessageType::ModifyOrder)));

  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 12u);
  EXPECT_EQ(top.bid_qty, 40u);

  ASSERT_NO_THROW(book.deleteOrder(makeMessage(
      large_order_id, 42, 0, 0, OrderSide::Buy, MessageType::DeleteOrder)));

  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
}

TEST(OrderBookTest, BookUpdateAfterAddingOrders) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 11, 50, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(3, 42, 12, 75, OrderSide::Sell, MessageType::AddOrder));
  book.addOrder(
      makeMessage(4, 42, 13, 25, OrderSide::Sell, MessageType::AddOrder));

  const BookUpdate update = book.getBookUpdate();

  EXPECT_EQ(update.symbol_id, 42u);
  EXPECT_EQ(update.bids_depth, 2u);
  EXPECT_EQ(update.asks_depth, 2u);
  EXPECT_EQ(update.bids[0].price, 11u);
  EXPECT_EQ(update.bids[0].qty, 50u);
  EXPECT_EQ(update.bids[1].price, 10u);
  EXPECT_EQ(update.bids[1].qty, 100u);
  EXPECT_EQ(update.asks[0].price, 12u);
  EXPECT_EQ(update.asks[0].qty, 75u);
  EXPECT_EQ(update.asks[1].price, 13u);
  EXPECT_EQ(update.asks[1].qty, 25u);
}

TEST(OrderBookTest, MultipleOrdersAtSamePriceAggregateQuantity) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 10, 40, OrderSide::Buy, MessageType::AddOrder));

  TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 140u);

  book.deleteOrder(
      makeMessage(1, 42, 0, 0, OrderSide::Buy, MessageType::DeleteOrder));

  top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 40u);
}

TEST(OrderBookTest, PartialTradeReducesBestLevelQuantity) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.trade(makeMessage(1, 42, 0, 25, OrderSide::Buy, MessageType::Trade));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 75u);
}

TEST(OrderBookTest, FullTradeFallsBackToNextBestBid) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 11, 50, OrderSide::Buy, MessageType::AddOrder));

  book.trade(makeMessage(2, 42, 0, 50, OrderSide::Buy, MessageType::Trade));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 100u);
}

TEST(OrderBookTest, TradeLargerThanOrderQuantityIsIgnored) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(2, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));

  book.trade(makeMessage(1, 42, 0, 150, OrderSide::Buy, MessageType::Trade));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 200u);
}

TEST(OrderBookTest, DuplicateActiveOrderIdIsIgnored) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.addOrder(
      makeMessage(1, 42, 11, 50, OrderSide::Buy, MessageType::AddOrder));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 100u);

  book.deleteOrder(
      makeMessage(1, 42, 0, 0, OrderSide::Buy, MessageType::DeleteOrder));

  const TopOfBook top_after_delete = book.getTopOfBook();
  EXPECT_EQ(top_after_delete.bid_price, 0u);
  EXPECT_EQ(top_after_delete.bid_qty, 0u);
}

TEST(OrderBookTest, ZeroQuantityAddIsIgnored) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 0, OrderSide::Buy, MessageType::AddOrder));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
}

TEST(OrderBookTest, ModifyToZeroQuantityRemovesOrder) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.modifyOrder(
      makeMessage(1, 42, 10, 0, OrderSide::Buy, MessageType::ModifyOrder));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
}

TEST(OrderBookTest, ModifyOrderCanMoveBetweenSides) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));
  book.modifyOrder(
      makeMessage(1, 42, 12, 80, OrderSide::Sell, MessageType::ModifyOrder));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
  EXPECT_EQ(top.ask_price, 12u);
  EXPECT_EQ(top.ask_qty, 80u);
}

TEST(OrderBookTest, MissingOrderOperationsDoNotChangeBook) {
  OrderBook book(42);

  book.addOrder(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::AddOrder));

  book.modifyOrder(
      makeMessage(2, 42, 11, 50, OrderSide::Buy, MessageType::ModifyOrder));
  book.deleteOrder(
      makeMessage(3, 42, 0, 0, OrderSide::Buy, MessageType::DeleteOrder));
  book.trade(makeMessage(4, 42, 0, 50, OrderSide::Buy, MessageType::Trade));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 100u);
}

TEST(OrderBookTest, BookUpdateCapsAtTopTenLevels) {
  OrderBook book(42);

  for (uint32_t i = 0; i < 12; ++i) {
    book.addOrder(makeMessage(100 + i, 42, 10 + i, 10 + i, OrderSide::Buy,
                              MessageType::AddOrder));
    book.addOrder(makeMessage(200 + i, 42, 20 + i, 20 + i, OrderSide::Sell,
                              MessageType::AddOrder));
  }

  const BookUpdate update = book.getBookUpdate();

  ASSERT_EQ(update.bids_depth, 10u);
  ASSERT_EQ(update.asks_depth, 10u);
  EXPECT_EQ(update.bids[0].price, 21u);
  EXPECT_EQ(update.bids[9].price, 12u);
  EXPECT_EQ(update.asks[0].price, 20u);
  EXPECT_EQ(update.asks[9].price, 29u);
}
