#include "astra/book/BookManager.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <gtest/gtest.h>

TEST(BookManagerTest, UnknownLocateReturnsNull) {
  BookManager manager;
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, RoutesOrdersByStockLocate) {
  BookManager manager;
  manager.addOrder(7, 1, 100, 10, OrderSide::Buy);
  manager.addOrder(9, 2, 200, 20, OrderSide::Sell);

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

TEST(BookManagerTest, DeleteForUnknownLocateIsIgnored) {
  BookManager manager;
  manager.deleteOrder(42, 999); // no book for locate 42 yet — no crash
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, DeleteRemovesOrder) {
  BookManager manager;
  manager.addOrder(1, 10, 50, 100, OrderSide::Buy);
  manager.deleteOrder(1, 10);

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 0u);
}

TEST(BookManagerTest, CancelSharesReducesQuantity) {
  BookManager manager;
  manager.addOrder(1, 10, 50, 100, OrderSide::Buy);
  manager.cancelShares(1, 10, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty, 40u);
}

TEST(BookManagerTest, TradeReducesQuantity) {
  BookManager manager;
  manager.addOrder(1, 10, 50, 100, OrderSide::Buy);
  manager.trade(1, 10, 30);

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty, 70u);
}

TEST(BookManagerTest, ReplaceOrderUpdatesBook) {
  BookManager manager;
  manager.addOrder(1, 10, 50, 100, OrderSide::Buy);
  manager.replaceOrder(1, 10, 20, 60, 75); // old=10 → new=20 at price 60 qty 75

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 60u);
  EXPECT_EQ(top.bid_qty, 75u);
}

TEST(BookManagerTest, ZeroLocateIsIgnored) {
  BookManager manager;
  manager.addOrder(0, 1, 100, 50, OrderSide::Buy); // locate 0 is invalid
  EXPECT_EQ(manager.getOrderBook(0), nullptr);
}

TEST(BookManagerTest, AppliesConfiguredSymbolCapacityBeforeBookCreation) {
  BookManager manager;
  manager.setSymbolTier(7, astra::capacity::SymbolTier::Active);

  manager.addOrder(7, 1, 100, 10, OrderSide::Buy);

  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), astra::capacity::kActiveLiveOrderCapacity);
}

TEST(BookManagerTest, PrepareBookCreatesEmptyBookWithConfiguredCapacity) {
  BookManager manager;
  manager.setSymbolTier(7, astra::capacity::SymbolTier::Active);

  manager.prepareBook(7, /*channel_id=*/2);

  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), astra::capacity::kActiveLiveOrderCapacity);
  EXPECT_EQ(book->channelId(), 2u);
  EXPECT_EQ(book->liveOrderCount(), 0u);
}

TEST(BookManagerTest, UsesDefaultCapacityForUntieredSymbols) {
  BookManager manager;

  manager.addOrder(8, 1, 100, 10, OrderSide::Buy);

  const OrderBook *book = manager.getOrderBook(8);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), astra::capacity::kDefaultLiveOrderCapacity);
}
