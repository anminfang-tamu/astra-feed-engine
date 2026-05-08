#include "astra/book/BookManager.hpp"

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

TEST(BookManagerTest, UnknownSymbolReturnsNull) {
  BookManager manager;

  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, RoutesMessagesBySymbolId) {
  BookManager manager;

  manager.onMessage(
      makeMessage(1, 7, 100, 10, OrderSide::Buy, MessageType::AddOrder));
  manager.onMessage(
      makeMessage(2, 9, 200, 20, OrderSide::Sell, MessageType::AddOrder));

  const OrderBook *book7 = manager.getOrderBook(7);
  const OrderBook *book9 = manager.getOrderBook(9);

  ASSERT_NE(book7, nullptr);
  ASSERT_NE(book9, nullptr);

  const TopOfBook top7 = book7->getTopOfBook();
  EXPECT_EQ(top7.symbol_id, 7u);
  EXPECT_EQ(top7.bid_price, 100u);
  EXPECT_EQ(top7.bid_qty, 10u);
  EXPECT_EQ(top7.ask_price, 0u);
  EXPECT_EQ(top7.ask_qty, 0u);

  const TopOfBook top9 = book9->getTopOfBook();
  EXPECT_EQ(top9.symbol_id, 9u);
  EXPECT_EQ(top9.bid_price, 0u);
  EXPECT_EQ(top9.bid_qty, 0u);
  EXPECT_EQ(top9.ask_price, 200u);
  EXPECT_EQ(top9.ask_qty, 20u);
}

TEST(BookManagerTest, IgnoresNonBookMessages) {
  BookManager manager;

  manager.onMessage(
      makeMessage(0, 42, 0, 0, OrderSide::Buy, MessageType::Heartbeat));

  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, IgnoresUpdateForUnknownSymbol) {
  BookManager manager;

  manager.onMessage(
      makeMessage(1, 42, 10, 100, OrderSide::Buy, MessageType::ModifyOrder));

  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}
