#include "astra/book/BookManager.hpp"
#include "astra/book/BookCapacity.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/symbol/SymbolTier.hpp"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kTestMaxDirectOrderRef = 1024;

} // namespace

TEST(BookManagerTest, UnknownLocateReturnsNull) {
  BookManager manager(kTestMaxDirectOrderRef);
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, RoutesOrdersByStockLocate) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.addOrder(7,  1, 100, 10, 'B');
  manager.addOrder(9,  2, 200, 20, 'S');

  const OrderBook *book7 = manager.getOrderBook(7);
  const OrderBook *book9 = manager.getOrderBook(9);

  ASSERT_NE(book7, nullptr);
  ASSERT_NE(book9, nullptr);

  const TopOfBook top7 = book7->getTopOfBook();
  EXPECT_EQ(top7.symbol_id,  7u);
  EXPECT_EQ(top7.bid_price, 100u);
  EXPECT_EQ(top7.bid_qty,   10u);
  EXPECT_EQ(top7.ask_price,  0u);
  EXPECT_EQ(top7.ask_qty,    0u);

  const TopOfBook top9 = book9->getTopOfBook();
  EXPECT_EQ(top9.symbol_id,  9u);
  EXPECT_EQ(top9.bid_price,   0u);
  EXPECT_EQ(top9.bid_qty,     0u);
  EXPECT_EQ(top9.ask_price, 200u);
  EXPECT_EQ(top9.ask_qty,   20u);
}

TEST(BookManagerTest, DeleteForUnknownLocateIsIgnored) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.deleteOrder(42, 999); // no book for locate 42 yet — no crash
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, DeleteRemovesOrder) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.deleteOrder(1, 10);

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(BookManagerTest, CancelSharesReducesQuantity) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.cancelShares(1, 10, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(BookManagerTest, TradeReducesQuantity) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.addOrder(1, 10, 50, 100, 'B');
  EXPECT_TRUE(manager.trade(1, 10, 30));

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty,   70u);
}

TEST(BookManagerTest, ReplaceOrderUpdatesBook) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.replaceOrder(1, 10, 20, 60, 75); // old=10 → new=20 at price 60 qty 75

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 60u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(BookManagerTest, HandlesDirectOrderRefAtConfiguredLimit) {
  constexpr uint64_t order_id = kTestMaxDirectOrderRef;
  constexpr uint64_t replacement_id = order_id + 1;
  BookManager manager(kTestMaxDirectOrderRef + 1);

  manager.addOrder(7, order_id, 100, 100, 'B');
  manager.cancelShares(7, order_id, 25);

  TopOfBook top = manager.getOrderBook(7)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 100u);
  EXPECT_EQ(top.bid_qty,   75u);

  manager.replaceOrder(7, order_id, replacement_id, 101, 50);
  EXPECT_TRUE(manager.trade(7, replacement_id, 50));

  top = manager.getOrderBook(7)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(BookManagerTest, FallsBackForSparseOrderRefOutsideDirectRange) {
  constexpr uint64_t order_id = 9'000'000'000'000'000'000ULL;
  BookManager manager(kTestMaxDirectOrderRef);

  manager.addOrder(7, order_id, 100, 100, 'B');
  manager.cancelShares(7, order_id, 25);

  TopOfBook top = manager.getOrderBook(7)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 100u);
  EXPECT_EQ(top.bid_qty,   75u);

  manager.deleteOrder(7, order_id);

  top = manager.getOrderBook(7)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(BookManagerTest, ZeroLocateIsIgnored) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.addOrder(0, 1, 100, 50, 'B'); // locate 0 is invalid
  EXPECT_EQ(manager.getOrderBook(0), nullptr);
}

TEST(BookManagerTest, AppliesConfiguredBookCapacityBeforeBookCreation) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::Active);

  manager.addOrder(7, 1, 100, 10, 'B');

  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), astra::book_capacity::kActiveOrderCapacity);
}

TEST(BookManagerTest, GetOrCreateCreatesEmptyBookWithConfiguredCapacity) {
  BookManager manager(kTestMaxDirectOrderRef);
  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::Active);

  (void)manager.getOrCreate(7);

  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), astra::book_capacity::kActiveOrderCapacity);
  EXPECT_EQ(book->liveOrderCount(), 0u);
}

TEST(BookManagerTest, UsesDefaultCapacityForUntieredSymbols) {
  BookManager manager(kTestMaxDirectOrderRef);

  manager.addOrder(8, 1, 100, 10, 'B');

  const OrderBook *book = manager.getOrderBook(8);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), astra::book_capacity::kDefaultOrderCapacity);
}
