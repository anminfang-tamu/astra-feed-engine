#include "astra/book/BookManager.hpp"
#include "astra/book/TopOfBook.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace {

constexpr size_t kTestOrderCapacity = 64;
constexpr PriceLevelArenaConfig kTestPriceLevelConfig{256, 128, 128};

} // namespace

TEST(BookManagerTest, UnknownLocateReturnsNull) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, RejectsInvalidFixedPoolCapacities) {
  EXPECT_THROW((BookManager(0, {2, 1, 1})), std::invalid_argument);
  EXPECT_THROW(
      (BookManager(8,
                   {2, 1, static_cast<size_t>(Order::kLevelMask) + 1})),
      std::invalid_argument);
}

TEST(BookManagerTest, RoutesOrdersByStockLocate) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(7, 1, 100, 10, 'B');
  manager.addOrder(9, 2, 200, 20, 'S');

  const OrderBook *book7 = manager.getOrderBook(7);
  const OrderBook *book9 = manager.getOrderBook(9);
  ASSERT_NE(book7, nullptr);
  ASSERT_NE(book9, nullptr);

  const TopOfBook top7 = book7->getTopOfBook();
  EXPECT_EQ(top7.symbol_id, 7u);
  EXPECT_EQ(top7.bid_price, 100u);
  EXPECT_EQ(top7.bid_qty, 10u);
  EXPECT_FALSE(top7.has_ask);

  const TopOfBook top9 = book9->getTopOfBook();
  EXPECT_EQ(top9.symbol_id, 9u);
  EXPECT_EQ(top9.ask_price, 200u);
  EXPECT_EQ(top9.ask_qty, 20u);
  EXPECT_FALSE(top9.has_bid);
}

TEST(BookManagerTest, RoutesMutationsIntoOwningBook) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.cancelShares(1, 10, 20);
  EXPECT_TRUE(manager.trade(1, 10, 30));
  manager.replaceOrder(1, 10, 20, 60, 75);

  TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 60u);
  EXPECT_EQ(top.bid_qty, 75u);

  manager.deleteOrder(1, 20);
  top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_FALSE(top.has_bid);
}

TEST(BookManagerTest, HandlesFullUint64OrderRefsLocally) {
  constexpr uint64_t order_id = uint64_t{1} << 63;
  constexpr uint64_t replacement_id = std::numeric_limits<uint64_t>::max();
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);

  manager.addOrder(7, order_id, 100, 100, 'B');
  manager.cancelShares(7, order_id, 25);
  manager.replaceOrder(7, order_id, replacement_id, 101, 50);
  EXPECT_TRUE(manager.trade(7, replacement_id, 50));

  const TopOfBook top = manager.getOrderBook(7)->getTopOfBook();
  EXPECT_FALSE(top.has_bid);
}

TEST(BookManagerTest, ZeroLocateIsIgnored) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(0, 1, 100, 50, 'B');
  EXPECT_EQ(manager.getOrderBook(0), nullptr);
  EXPECT_EQ(manager.stats().invalid_adds, 1u);
}

TEST(BookManagerTest, InvalidAddDoesNotAllocateABook) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);

  manager.addOrder(7, 0, 100, 10, 'B');
  manager.addOrder(7, 1, 100, 0, 'B');
  manager.addOrder(7, 2, 100, 10, 'X');

  EXPECT_EQ(manager.getOrderBook(7), nullptr);
  EXPECT_EQ(manager.stats().invalid_adds, 3u);
}

TEST(BookManagerTest, InvalidReverseDoesNotAllocateABook) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);

  manager.reverseExecution(7, 0, 100, 10, 'B');
  manager.reverseExecution(7, 1, 100, 0, 'B');
  manager.reverseExecution(7, 2, 100, 10, 'X');

  EXPECT_EQ(manager.getOrderBook(7), nullptr);
  EXPECT_EQ(manager.stats().mutation_failures, 3u);
}

TEST(BookManagerTest, UnknownBookMutationIsCountedAsMissing) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.deleteOrder(42, 999);
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
  EXPECT_EQ(manager.stats().missing_order_refs, 1u);
}

TEST(BookManagerTest, PrepareBookConstructsEmptyLocalStorage) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(manager.prepareBook(7));
  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), kTestOrderCapacity);
  EXPECT_EQ(book->orderIndexCapacity(), kTestOrderCapacity * 4);
  EXPECT_EQ(book->liveOrderCount(), 0u);

  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.books, 1u);
  EXPECT_EQ(stats.orders.capacity, kTestOrderCapacity);
  EXPECT_EQ(stats.orders.in_use, 0u);
  EXPECT_EQ(stats.order_ref_index_size, 0u);
  EXPECT_EQ(stats.inconsistent_books, 0u);
}

TEST(BookManagerTest, ValidationStatsSkipDetailedIndexProbeScan) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(7, 1, 100, 10, 'B');

  const BookManagerStats validation =
      manager.stats(BookStatsDetail::ValidationOnly);
  const BookManagerStats full = manager.stats();

  EXPECT_EQ(validation.live_orders, full.live_orders);
  EXPECT_EQ(validation.order_ref_index_size, full.order_ref_index_size);
  EXPECT_EQ(validation.orders.in_use, full.orders.in_use);
  EXPECT_EQ(validation.inconsistent_books, full.inconsistent_books);
  EXPECT_EQ(validation.order_ref_index_max_probe_length, 0u);
  EXPECT_GE(full.order_ref_index_max_probe_length, 1u);
}

TEST(BookManagerTest, ExhaustedBookCanDeleteAndReuseLocalCapacity) {
  BookManager manager(2, {32, 16, 16});
  manager.addOrder(7, 1, 100, 10, 'B');
  manager.addOrder(7, 2, 101, 20, 'B');
  manager.addOrder(7, 3, 102, 30, 'B');

  ASSERT_EQ(manager.getOrder(7, 3), nullptr);
  manager.deleteOrder(7, 1);
  manager.addOrder(7, 4, 103, 40, 'B');

  EXPECT_EQ(manager.getOrder(7, 1), nullptr);
  EXPECT_NE(manager.getOrder(7, 2), nullptr);
  EXPECT_NE(manager.getOrder(7, 4), nullptr);
  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.live_orders, 2u);
  EXPECT_EQ(stats.order_ref_index_size, 2u);
  EXPECT_EQ(stats.orders.in_use, 2u);
  EXPECT_EQ(stats.orders.allocation_failures, 1u);
  EXPECT_EQ(stats.inconsistent_books, 0u);
}

TEST(BookManagerTest, IsolatesFixedOrderCapacityPerBook) {
  BookManager manager(2, {16, 8, 8});

  manager.addOrder(7, 1, 100, 10, 'B');
  manager.addOrder(7, 2, 101, 20, 'B');
  manager.addOrder(7, 3, 102, 30, 'B');
  manager.addOrder(8, 1, 200, 40, 'S');

  EXPECT_NE(manager.getOrder(7, 1), nullptr);
  EXPECT_NE(manager.getOrder(7, 2), nullptr);
  EXPECT_EQ(manager.getOrder(7, 3), nullptr);
  EXPECT_NE(manager.getOrder(8, 1), nullptr);

  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.orders.capacity, 4u);
  EXPECT_EQ(stats.orders.in_use, 3u);
  EXPECT_EQ(stats.orders.high_watermark, 3u);
  EXPECT_EQ(stats.orders.allocation_failures, 1u);
  EXPECT_EQ(stats.live_orders, 3u);
  EXPECT_EQ(stats.order_ref_index_size, 3u);
  EXPECT_EQ(stats.inconsistent_books, 0u);
  EXPECT_TRUE(manager.getOrderBook(7)->localInvalid());
  EXPECT_FALSE(manager.getOrderBook(8)->localInvalid());
}

TEST(BookManagerTest, SameReferenceIsScopedByStockLocate) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(7, 42, 100, 10, 'B');
  manager.addOrder(8, 42, 200, 20, 'S');

  ASSERT_NE(manager.getOrder(7, 42), nullptr);
  ASSERT_NE(manager.getOrder(8, 42), nullptr);
  EXPECT_EQ(manager.getOrder(7, 42)->price, 100u);
  EXPECT_EQ(manager.getOrder(8, 42)->price, 200u);

  manager.deleteOrder(7, 42);
  EXPECT_EQ(manager.getOrder(7, 42), nullptr);
  EXPECT_NE(manager.getOrder(8, 42), nullptr);
}

TEST(BookManagerTest, WrongLocateMutationLeavesOwningBookUntouched) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(7, 42, 100, 10, 'B');
  ASSERT_TRUE(manager.prepareBook(8));

  manager.deleteOrder(8, 42);

  const Order *owner_order = manager.getOrder(7, 42);
  ASSERT_NE(owner_order, nullptr);
  EXPECT_EQ(owner_order->qty, 10u);
  EXPECT_FALSE(manager.getOrderBook(7)->localInvalid());
  EXPECT_TRUE(manager.getOrderBook(8)->localInvalid());
  EXPECT_EQ(manager.stats().missing_order_refs, 1u);
}

TEST(BookManagerTest, GetOrderRetainsPriceForStrategyConsumers) {
  constexpr uint64_t order_id = 8'765'432'109'876'543'210ULL;
  constexpr uint64_t price = 1'234'567ULL;
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(7, order_id, price, 250, 'S');

  const Order *order = manager.getOrder(7, order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_id, order_id);
  EXPECT_EQ(order->price, price);
  EXPECT_EQ(order->qty, 250u);
  EXPECT_EQ(order->side(), 'S');
}

TEST(BookManagerTest, PriceLevelCapacityFailureIsExplicitAndAtomic) {
  BookManager manager(16, {2, 1, 1});
  manager.addOrder(7, 1, 100, 10, 'B');
  ASSERT_NE(manager.getOrder(7, 1), nullptr);
  manager.addOrder(7, 2, 101, 20, 'B');

  EXPECT_EQ(manager.getOrder(7, 2), nullptr);
  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->liveOrderCount(), 1u);
  EXPECT_EQ(book->getTopOfBook().bid_price, 100u);
  EXPECT_TRUE(book->localInvalid());

  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.live_orders, 1u);
  EXPECT_EQ(stats.order_ref_index_size, 1u);
  EXPECT_EQ(stats.price_rejections, 1u);
  EXPECT_EQ(stats.price_levels.levels_in_use, 1u);
  EXPECT_EQ(stats.price_levels.level_exhaustions, 1u);
  EXPECT_EQ(stats.inconsistent_books, 0u);
}

TEST(BookManagerTest, SharedPriceCapacityIsReleasedForAnotherBook) {
  BookManager manager(4, {2, 1, 1});
  manager.addOrder(7, 1, 100, 10, 'B');
  manager.addOrder(8, 2, 200, 20, 'S');
  ASSERT_EQ(manager.getOrder(8, 2), nullptr);

  manager.deleteOrder(7, 1);
  manager.addOrder(8, 2, 200, 20, 'S');

  EXPECT_EQ(manager.getOrder(7, 1), nullptr);
  ASSERT_NE(manager.getOrder(8, 2), nullptr);
  EXPECT_EQ(manager.getOrderBook(8)->getTopOfBook().ask_price, 200u);
  const BookManagerStats stats = manager.stats();
  EXPECT_EQ(stats.live_orders, 1u);
  EXPECT_EQ(stats.order_ref_index_size, 1u);
  EXPECT_EQ(stats.price_levels.levels_in_use, 1u);
  EXPECT_EQ(stats.inconsistent_books, 0u);
}

TEST(BookManagerTest, ReverseExecutionRejectsPriceOrSideMismatch) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  manager.addOrder(7, 42, 100, 10, 'B');

  manager.reverseExecution(7, 42, 101, 5, 'B');

  const Order *order = manager.getOrder(7, 42);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->qty, 10u);
  EXPECT_TRUE(manager.getOrderBook(7)->localInvalid());
  EXPECT_EQ(manager.stats().mutation_failures, 1u);
}

TEST(BookManagerTest, CoversCompleteUint32PriceDomain) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  constexpr uint64_t max_price = std::numeric_limits<uint32_t>::max();
  manager.addOrder(7, 1, 0, 10, 'B');
  manager.addOrder(7, 2, max_price, 20, 'B');
  manager.addOrder(7, 3, 0, 30, 'S');
  manager.addOrder(7, 4, max_price, 40, 'S');

  const TopOfBook top = manager.getOrderBook(7)->getTopOfBook();
  EXPECT_TRUE(top.has_bid);
  EXPECT_TRUE(top.has_ask);
  EXPECT_EQ(top.bid_price, max_price);
  EXPECT_EQ(top.ask_price, 0u);
}

TEST(BookManagerTest, RejectsPricesOutsideItchUint32Domain) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  constexpr uint64_t invalid_price =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
  manager.addOrder(7, 1, invalid_price, 10, 'B');

  EXPECT_EQ(manager.getOrder(7, 1), nullptr);
  ASSERT_NE(manager.getOrderBook(7), nullptr);
  EXPECT_TRUE(manager.getOrderBook(7)->localInvalid());
  EXPECT_EQ(manager.stats().price_rejections, 1u);
}

TEST(BookManagerTest, AppliesPerLocateCapacityBeforeConstruction) {
  BookManager manager(8, {32, 16, 16});
  manager.setBookOrderCapacity(7, 2);
  manager.setBookCapacityTier(8, astra::symbol::SymbolTier::Active);

  ASSERT_TRUE(manager.prepareBook(7));
  ASSERT_TRUE(manager.prepareBook(8));
  EXPECT_EQ(manager.getOrderBook(7)->orderCapacity(), 2u);
  EXPECT_EQ(manager.getOrderBook(8)->orderCapacity(),
            astra::book_capacity::kActiveOrderCapacity);
}

TEST(BookManagerTest, ExplicitPerLocateCapacityWinsOverTickerTier) {
  BookManager manager(8, {32, 16, 16});
  manager.setBookOrderCapacity(7, 2);
  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::UltraHot);

  ASSERT_TRUE(manager.prepareBook(7));
  EXPECT_EQ(manager.getOrderBook(7)->orderCapacity(), 2u);
}

TEST(BookManagerTest, TierPolicyCanBeRevisedBeforeConstruction) {
  BookManager manager(8, {32, 16, 16});
  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::Active);
  EXPECT_EQ(manager.orderCapacityForLocate(7),
            astra::book_capacity::kActiveOrderCapacity);

  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::Hot);
  EXPECT_EQ(manager.orderCapacityForLocate(7),
            astra::book_capacity::kHotOrderCapacity);

  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::Default);
  EXPECT_EQ(manager.orderCapacityForLocate(7), 8u);
}

TEST(BookManagerTest, DefaultTierUsesConfiguredManagerDefault) {
  BookManager manager(32, {64, 32, 32});
  manager.setBookCapacityTier(7, astra::symbol::SymbolTier::Default);

  ASSERT_TRUE(manager.prepareBook(7));
  EXPECT_EQ(manager.getOrderBook(7)->orderCapacity(), 32u);
}

TEST(BookManagerTest, SealedBookUniverseRejectsLateAllocation) {
  BookManager manager(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(manager.prepareBook(7));
  manager.sealBookUniverse();

  EXPECT_FALSE(manager.prepareBook(8));
  EXPECT_TRUE(manager.prepareBook(7));
  EXPECT_TRUE(manager.bookUniverseSealed());
  EXPECT_EQ(manager.stats().late_book_creation_attempts, 1u);
}
