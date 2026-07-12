#include "astra/book/BookManager.hpp"
#include "astra/book/TopOfBook.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace {

constexpr size_t kTestDirectorySlots = 1024;

} // namespace

TEST(BookManagerTest, UnknownLocateReturnsNull) {
  BookManager manager(kTestDirectorySlots);
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, RejectsIncoherentFixedPoolCapacities) {
  EXPECT_THROW((BookManager(8, {2, 1, 1}, 5)), std::invalid_argument);
  EXPECT_THROW((BookManager(
                   8, {2, 1, static_cast<size_t>(Order::kLevelMask) + 1}, 4)),
               std::invalid_argument);
}

TEST(BookManagerTest, RoutesOrdersByStockLocate) {
  BookManager manager(kTestDirectorySlots);
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
  BookManager manager(kTestDirectorySlots);
  manager.deleteOrder(42, 999); // no book for locate 42 yet — no crash
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, DeleteRemovesOrder) {
  BookManager manager(kTestDirectorySlots);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.deleteOrder(1, 10);

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(BookManagerTest, CancelSharesReducesQuantity) {
  BookManager manager(kTestDirectorySlots);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.cancelShares(1, 10, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(BookManagerTest, TradeReducesQuantity) {
  BookManager manager(kTestDirectorySlots);
  manager.addOrder(1, 10, 50, 100, 'B');
  EXPECT_TRUE(manager.trade(1, 10, 30));

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty,   70u);
}

TEST(BookManagerTest, ReplaceOrderUpdatesBook) {
  BookManager manager(kTestDirectorySlots);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.replaceOrder(1, 10, 20, 60, 75); // old=10 → new=20 at price 60 qty 75

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 60u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(BookManagerTest, HandlesFullUint64OrderRefsAcrossReplaceAndTrade) {
  constexpr uint64_t order_id = uint64_t{1} << 63;
  constexpr uint64_t replacement_id =
      std::numeric_limits<uint64_t>::max();
  BookManager manager(kTestDirectorySlots);

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

TEST(BookManagerTest, HandlesSparseFullUint64OrderRefWithoutFallback) {
  constexpr uint64_t order_id = 9'000'000'000'000'000'000ULL;
  BookManager manager(kTestDirectorySlots);

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
  BookManager manager(kTestDirectorySlots);
  manager.addOrder(0, 1, 100, 50, 'B'); // locate 0 is invalid
  EXPECT_EQ(manager.getOrderBook(0), nullptr);
}

TEST(BookManagerTest, GetOrCreateDoesNotConsumeGlobalOrderPool) {
  BookManager manager(kTestDirectorySlots);

  (void)manager.getOrCreate(7);

  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->liveOrderCount(), 0u);
  EXPECT_EQ(manager.stats().orders.in_use, 0u);
}

TEST(BookManagerTest, SharesOneFixedOrderPoolAcrossBooks) {
  BookManager manager(16, {16, 8, 8}, 2);

  manager.addOrder(7, 1, 100, 10, 'B');
  manager.addOrder(8, 2, 200, 20, 'S');
  manager.addOrder(9, 3, 300, 30, 'B');

  EXPECT_NE(manager.getOrder(1), nullptr);
  EXPECT_NE(manager.getOrder(2), nullptr);
  EXPECT_EQ(manager.getOrder(3), nullptr);
  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.orders.capacity, 2u);
  EXPECT_EQ(stats.orders.in_use, 2u);
  EXPECT_EQ(stats.orders.high_watermark, 2u);
  EXPECT_EQ(stats.orders.allocation_failures, 1u);
  EXPECT_EQ(stats.allocation_failures, 1u);
  EXPECT_EQ(stats.live_orders, 2u);
  EXPECT_EQ(stats.directory_size, 2u);
  ASSERT_NE(manager.getOrderBook(9), nullptr);
  EXPECT_TRUE(manager.getOrderBook(9)->localInvalid());
}

TEST(BookManagerTest, GetOrderRetainsPriceForStrategyConsumers) {
  constexpr uint64_t order_id = 8'765'432'109'876'543'210ULL;
  constexpr uint64_t price = 1'234'567ULL;
  BookManager manager(kTestDirectorySlots);

  manager.addOrder(7, order_id, price, 250, 'S');

  const Order *order = manager.getOrder(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_id, order_id);
  EXPECT_EQ(order->price, price);
  EXPECT_EQ(order->qty, 250u);
  EXPECT_EQ(order->side(), 'S');

  manager.deleteOrder(7, order_id);
  EXPECT_EQ(manager.getOrder(order_id), nullptr);
}

TEST(BookManagerTest, DirectoryCapacityFailureRejectsBeforeOrderAllocation) {
  BookManager manager(4, {8, 4, 8});
  // The directory half-load invariant permits two live entries; the price
  // arena deliberately has headroom so this isolates directory exhaustion.

  manager.addOrder(7, 1, 100, 10, 'B');
  manager.addOrder(7, 2, 101, 20, 'B');
  ASSERT_NE(manager.getOrder(1), nullptr);
  ASSERT_NE(manager.getOrder(2), nullptr);

  manager.addOrder(7, 3, 102, 30, 'B');

  EXPECT_EQ(manager.getOrder(3), nullptr);
  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->liveOrderCount(), 2u);
  const TopOfBook top = book->getTopOfBook();
  EXPECT_EQ(top.bid_price, 101u);
  EXPECT_EQ(top.bid_qty, 20u);
  EXPECT_TRUE(book->localInvalid());

  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.books, 1u);
  EXPECT_EQ(stats.live_orders, 2u);
  EXPECT_EQ(stats.directory_size, 2u);
  EXPECT_EQ(stats.directory_capacity, 4u);
  EXPECT_EQ(stats.directory_max_entries, 2u);
  EXPECT_EQ(stats.directory_insert_failures, 1u);
  EXPECT_EQ(stats.mutation_failures, 1u);
  EXPECT_EQ(stats.locally_invalid_books, 1u);
}

TEST(BookManagerTest, PriceLevelCapacityFailureIsExplicitAndAtomic) {
  BookManager manager(16, {2, 1, 1});

  manager.addOrder(7, 1, 100, 10, 'B');
  ASSERT_NE(manager.getOrder(1), nullptr);
  manager.addOrder(7, 2, 101, 20, 'B');

  EXPECT_EQ(manager.getOrder(2), nullptr);
  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  const TopOfBook top = book->getTopOfBook();
  EXPECT_EQ(top.bid_price, 100u);
  EXPECT_EQ(top.bid_qty, 10u);
  EXPECT_TRUE(book->localInvalid());

  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.live_orders, 1u);
  EXPECT_EQ(stats.directory_size, 1u);
  EXPECT_EQ(stats.price_rejections, 1u);
  EXPECT_EQ(stats.price_levels.levels_in_use, 1u);
  EXPECT_EQ(stats.price_levels.level_exhaustions, 1u);
  EXPECT_EQ(stats.locally_invalid_books, 1u);
}

TEST(BookManagerTest, RejectsPricesOutsideItchUint32Domain) {
  BookManager manager(kTestDirectorySlots);
  constexpr uint64_t invalid_price =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;

  manager.addOrder(7, 1, invalid_price, 10, 'B');

  EXPECT_EQ(manager.getOrder(1), nullptr);
  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_TRUE(book->localInvalid());
  EXPECT_EQ(manager.stats().price_rejections, 1u);
}

TEST(BookManagerTest, SealedBookUniverseRejectsLateAllocation) {
  BookManager manager(kTestDirectorySlots);
  ASSERT_NE(manager.getOrCreate(7), nullptr);
  manager.sealBookUniverse();

  EXPECT_EQ(manager.getOrCreate(8), nullptr);
  EXPECT_NE(manager.getOrCreate(7), nullptr);
  EXPECT_TRUE(manager.bookUniverseSealed());
  EXPECT_EQ(manager.stats().late_book_creation_attempts, 1u);
}
