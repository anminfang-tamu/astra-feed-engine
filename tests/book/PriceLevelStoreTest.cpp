#include "astra/book/PriceLevelStore.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace astra::book {

struct PriceLevelStoreTestPeer {
  static void setRoot(PriceLevelStore &store, std::uint16_t stock_locate,
                      std::uint16_t page_index,
                      PricePageHandle handle) noexcept {
    store.roots_[static_cast<std::size_t>(
        priceRootIndex(stock_locate, page_index))] = handle;
  }

  static bool pageOccupancyEmpty(const PriceLevelStore &store,
                                 PricePageHandle handle,
                                 OrderSide side) noexcept {
    return PriceBitmap::empty(*store.pageOccupancy(handle, side));
  }

  static std::uint32_t activeLevels(const PriceLevelStore &store,
                                    PricePageHandle handle,
                                    OrderSide side) noexcept {
    return store.pageSummary(handle, side)->active_levels;
  }

  static void setPageSummary(PriceLevelStore &store,
                             PricePageHandle handle, OrderSide side,
                             std::uint32_t best_level_index,
                             std::uint32_t active_levels) noexcept {
    auto *summary = store.pageSummary(handle, side);
    summary->best_level_index = best_level_index;
    summary->active_levels = active_levels;
  }

  static void setBookBest(PriceLevelStore &store,
                          std::uint16_t stock_locate, OrderSide side,
                          std::uint32_t raw_price,
                          PricePageHandle handle) noexcept {
    auto *summary = store.bookSummary(stock_locate, side);
    summary->best_raw_price = raw_price;
    summary->best_page_handle = handle;
  }
};

} // namespace astra::book

namespace {

using astra::book::MutationResult;
using astra::book::PriceAddress;
using astra::book::PriceLevelState;
using astra::book::PriceLevelStore;
using astra::book::PriceLevelStoreConfig;

PriceAddress preparePrice(PriceLevelStore &store, std::uint16_t locate,
                          std::uint32_t price) {
  const auto reservation = store.reservePrice(locate, price);
  if (!reservation.valid()) {
    ADD_FAILURE() << "price reservation failed";
    return {};
  }
  if (store.commitReservation(reservation) != MutationResult::Applied) {
    ADD_FAILURE() << "price reservation commit failed";
    return {};
  }
  return reservation.address;
}

} // namespace

TEST(PriceLevelStoreTest, LayoutAndPriceAddressingCoverFullInternalUint32Domain) {
  using namespace astra::book;

  static_assert(sizeof(PriceLevelState) == 16);
  static_assert(alignof(PriceLevelState) == 16);
  static_assert(sizeof(PricePage) == 2u * 1024u * 1024u);
  static_assert(offsetof(PricePage, asks) == 1024u * 1024u);
  static_assert(sizeof(PriceBitmapStorage) == 8'384u);
  static_assert(sizeof(PriceLevelCursor) == 12u);
  static_assert(64 % PriceLevelStore::kPageSummaryStorageBytes == 0);
  static_assert(64 % PriceLevelStore::kBookSummaryStorageBytes == 0);

  constexpr std::array<std::uint32_t, 4> prices{
      0u, 65'535u, 65'536u, std::numeric_limits<std::uint32_t>::max()};
  for (const std::uint32_t price : prices) {
    const PriceParts parts = splitPrice(price);
    EXPECT_EQ(joinPrice(parts.page_index, parts.level_index), price);
  }

  EXPECT_EQ(priceRootIndex(65'535u, 65'535u), 0xffff'ffffULL);
  EXPECT_EQ(kPriceRootSlotCount, 0x1'0000'0000ULL);
}

TEST(PriceLevelStoreTest, CrossBookCachedBestAliasFailsReadTraversal) {
  using namespace astra::book;

  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  ASSERT_EQ(store.prepareBook(8), MutationResult::Applied);
  const PriceAddress first = preparePrice(store, 7, 100u);
  const PriceAddress second = preparePrice(store, 8, 200u);
  ASSERT_EQ(store.add(first, OrderSide::Buy, 10u), MutationResult::Applied);
  ASSERT_EQ(store.add(second, OrderSide::Buy, 20u), MutationResult::Applied);

  const PriceLevelCursor original = store.best(7, OrderSide::Buy);
  ASSERT_TRUE(original.valid);
  ASSERT_TRUE(store.view(original, OrderSide::Buy).valid);

  PriceLevelStoreTestPeer::setBookBest(store, 7, OrderSide::Buy, 200u,
                                       second.page_handle);
  const PriceLevelCursor corrupted = store.best(7, OrderSide::Buy);
  ASSERT_TRUE(corrupted.valid);
  EXPECT_EQ(corrupted.stock_locate, 7u);
  EXPECT_FALSE(store.view(corrupted, OrderSide::Buy).valid);
  EXPECT_FALSE(store.nextWorse(7, OrderSide::Buy, corrupted).valid);

  std::array<PriceLevelView, kTopLevelCount> output{};
  EXPECT_EQ(store.topTen(7, OrderSide::Buy, output), 0u);
  EXPECT_EQ(store.view(store.best(8, OrderSide::Buy), OrderSide::Buy).total_qty,
            20u);
}

TEST(PriceLevelStoreTest,
     SingletonPageAvoidsBitmapAndTwoLevelTransitionsStayExact) {
  using astra::book::PriceLevelStoreTestPeer;

  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress lower = preparePrice(store, 7, 100u);
  const PriceAddress higher = preparePrice(store, 7, 200u);
  ASSERT_EQ(lower.page_handle, higher.page_handle);
  static_assert(
      PriceLevelStore::kSingletonPageErasePageBitmapOperations == 0);

  ASSERT_EQ(store.add(lower, OrderSide::Buy, 10u), MutationResult::Applied);
  EXPECT_EQ(PriceLevelStoreTestPeer::activeLevels(
                store, lower.page_handle, OrderSide::Buy),
            1u);
  EXPECT_TRUE(PriceLevelStoreTestPeer::pageOccupancyEmpty(
      store, lower.page_handle, OrderSide::Buy));

  ASSERT_EQ(store.add(higher, OrderSide::Buy, 20u), MutationResult::Applied);
  EXPECT_EQ(PriceLevelStoreTestPeer::activeLevels(
                store, lower.page_handle, OrderSide::Buy),
            2u);
  EXPECT_FALSE(PriceLevelStoreTestPeer::pageOccupancyEmpty(
      store, lower.page_handle, OrderSide::Buy));

  ASSERT_EQ(store.erase(lower, OrderSide::Buy, 10u),
            MutationResult::Applied);
  EXPECT_EQ(PriceLevelStoreTestPeer::activeLevels(
                store, lower.page_handle, OrderSide::Buy),
            1u);
  EXPECT_TRUE(PriceLevelStoreTestPeer::pageOccupancyEmpty(
      store, lower.page_handle, OrderSide::Buy));
  EXPECT_EQ(store.best(7, OrderSide::Buy).raw_price, 200u);

  ASSERT_EQ(store.erase(higher, OrderSide::Buy, 20u),
            MutationResult::Applied);
  EXPECT_EQ(PriceLevelStoreTestPeer::activeLevels(
                store, lower.page_handle, OrderSide::Buy),
            0u);
  EXPECT_TRUE(PriceLevelStoreTestPeer::pageOccupancyEmpty(
      store, lower.page_handle, OrderSide::Buy));
  EXPECT_FALSE(store.best(7, OrderSide::Buy).valid);
}

TEST(PriceLevelStoreTest, ReservationDoesNotPublishUntilCommit) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);

  const auto reservation = store.reservePrice(7, 65'536u);
  ASSERT_TRUE(reservation.valid());
  EXPECT_TRUE(reservation.requires_commit);
  EXPECT_EQ(reservation.address.page_handle, 1u);
  EXPECT_EQ(store.pageHandle(7, 1), 0u);
  EXPECT_EQ(store.counters().committed_pages, 0u);

  EXPECT_EQ(store.commitReservation(reservation), MutationResult::Applied);
  EXPECT_EQ(store.pageHandle(7, 1), 1u);
  EXPECT_EQ(store.counters().committed_pages, 1u);
  const auto *page = store.page(1);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(page) %
                astra::book::kPricePageBytes,
            0u);
}

TEST(PriceLevelStoreTest, ExistingPageReservationIsIdempotent) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress first = preparePrice(store, 7, 123'456u);
  ASSERT_TRUE(first.valid());

  const auto again = store.reservePrice(7, 123'999u);
  ASSERT_TRUE(again.valid());
  EXPECT_FALSE(again.requires_commit);
  EXPECT_EQ(again.address.page_handle, first.page_handle);
  EXPECT_EQ(store.commitReservation(again), MutationResult::Applied);
  EXPECT_EQ(store.counters().committed_pages, 1u);

  ASSERT_EQ(store.add(first, OrderSide::Buy, 10u),
            MutationResult::Applied);
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Buy).raw_price, 123'456u);
  ASSERT_EQ(store.erase(first, OrderSide::Buy, 10u),
            MutationResult::Applied);
  const auto after_empty = store.reservePrice(7, 123'777u);
  ASSERT_TRUE(after_empty.valid());
  EXPECT_FALSE(after_empty.requires_commit);
  EXPECT_EQ(after_empty.address.page_handle, first.page_handle);
  EXPECT_EQ(store.counters().committed_pages, 1u);
}

TEST(PriceLevelStoreTest, PageAddressesAreTwoMiBAlignedAndStable) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress first = preparePrice(store, 7, 10u);
  const PriceAddress second = preparePrice(store, 7, 65'536u + 10u);
  ASSERT_EQ(first.page_handle, 1u);
  ASSERT_EQ(second.page_handle, 2u);

  const auto *first_page = store.page(first.page_handle);
  const auto *second_page = store.page(second.page_handle);
  ASSERT_NE(first_page, nullptr);
  ASSERT_NE(second_page, nullptr);
  const auto first_address = reinterpret_cast<std::uintptr_t>(first_page);
  const auto second_address = reinterpret_cast<std::uintptr_t>(second_page);
  EXPECT_EQ(first_address % astra::book::kPricePageBytes, 0u);
  EXPECT_EQ(second_address % astra::book::kPricePageBytes, 0u);
  EXPECT_EQ(second_address - first_address, astra::book::kPricePageBytes);

  ASSERT_EQ(store.add(first, OrderSide::Buy, 10u),
            MutationResult::Applied);
  ASSERT_EQ(store.add(second, OrderSide::Buy, 20u),
            MutationResult::Applied);
  EXPECT_EQ(store.page(first.page_handle), first_page);
  EXPECT_EQ(store.page(second.page_handle), second_page);
}

TEST(PriceLevelStoreTest, StaleReservationCannotPublishAnotherRoot) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);

  const auto first = store.reservePrice(7, 1u);
  const auto stale = store.reservePrice(7, 65'536u);
  ASSERT_TRUE(first.valid());
  ASSERT_TRUE(stale.valid());
  ASSERT_EQ(store.commitReservation(first), MutationResult::Applied);

  EXPECT_EQ(store.commitReservation(stale), MutationResult::StaleReservation);
  EXPECT_EQ(store.pageHandle(7, 1), 0u);
  EXPECT_EQ(store.counters().committed_pages, 1u);
}

TEST(PriceLevelStoreTest,
     CrossBookRootAliasFailsReservationWithoutMutatingOwner) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  ASSERT_EQ(store.prepareBook(8), MutationResult::Applied);
  constexpr std::uint32_t other_price = 65'600u;
  const PriceAddress other_book = preparePrice(store, 8, other_price);
  ASSERT_EQ(store.add(other_book, OrderSide::Buy, 100u),
            MutationResult::Applied);
  const auto counters_before = store.counters();

  astra::book::PriceLevelStoreTestPeer::setRoot(
      store, 7, astra::book::splitPrice(other_price).page_index,
      other_book.page_handle);

  EXPECT_EQ(store.reservePrice(7, other_price + 1).result,
            MutationResult::InvariantViolation);
  EXPECT_EQ(store.reservePrice(7, other_price + 2).result,
            MutationResult::InvariantViolation);
  EXPECT_EQ(store.pageHandle(7, 1), astra::book::kInvalidPricePageHandle);
  EXPECT_EQ(store.counters().committed_pages,
            counters_before.committed_pages);
  EXPECT_EQ(store.counters().page_capacity_failures,
            counters_before.page_capacity_failures);

  const PriceLevelState *level =
      store.resolveLevel(other_book, OrderSide::Buy);
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->total_qty, 100u);
  EXPECT_EQ(level->order_count, 1u);
  EXPECT_FALSE(store.best(7, OrderSide::Buy).valid);
  EXPECT_EQ(store.best(8, OrderSide::Buy).raw_price, other_price);
}

TEST(PriceLevelStoreTest, SameOwnerWrongPageRootFailsReservation) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress page_zero = preparePrice(store, 7, 100u);
  ASSERT_EQ(store.add(page_zero, OrderSide::Sell, 25u),
            MutationResult::Applied);

  astra::book::PriceLevelStoreTestPeer::setRoot(
      store, 7, 1, page_zero.page_handle);

  EXPECT_EQ(store.reservePrice(7, 65'600u).result,
            MutationResult::InvariantViolation);
  EXPECT_EQ(store.pageHandle(7, 1), astra::book::kInvalidPricePageHandle);
  const PriceLevelState *level =
      store.resolveLevel(page_zero, OrderSide::Sell);
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->total_qty, 25u);
  EXPECT_EQ(level->order_count, 1u);
}

TEST(PriceLevelStoreTest,
     OutOfRangeRootFailsReservationTraversalAndBestEraseAtomically) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress lower = preparePrice(store, 7, 100u);
  const PriceAddress higher = preparePrice(store, 7, 65'600u);
  ASSERT_EQ(store.add(lower, OrderSide::Buy, 10u), MutationResult::Applied);
  ASSERT_EQ(store.add(higher, OrderSide::Buy, 20u), MutationResult::Applied);
  const auto counters_before = store.counters();
  const auto best_before = store.best(7, OrderSide::Buy);
  ASSERT_TRUE(best_before.valid);

  astra::book::PriceLevelStoreTestPeer::setRoot(
      store, 7, 0, std::numeric_limits<astra::book::PricePageHandle>::max());

  EXPECT_EQ(store.pageHandle(7, 0), astra::book::kInvalidPricePageHandle);
  EXPECT_EQ(store.reservePrice(7, 101u).result,
            MutationResult::InvariantViolation);
  EXPECT_EQ(store.reservePrice(7, 102u).result,
            MutationResult::InvariantViolation);
  EXPECT_FALSE(store.nextWorse(7, OrderSide::Buy, best_before).valid);

  const PriceLevelState *lower_level =
      store.resolveLevel(lower, OrderSide::Buy);
  ASSERT_NE(lower_level, nullptr);
  EXPECT_EQ(lower_level->total_qty, 10u);
  EXPECT_EQ(lower_level->order_count, 1u);

  // Removing the valid best would require reconstructing it from the corrupt
  // lower root. Reject the mutation before changing either the level or the
  // cached best; corruption must not be converted into successful best loss.
  EXPECT_EQ(store.erase(higher, OrderSide::Buy, 20u),
            MutationResult::InvariantViolation);
  const auto best_after = store.best(7, OrderSide::Buy);
  ASSERT_TRUE(best_after.valid);
  EXPECT_EQ(best_after.page_handle, best_before.page_handle);
  EXPECT_EQ(best_after.raw_price, best_before.raw_price);
  const PriceLevelState *higher_level =
      store.resolveLevel(higher, OrderSide::Buy);
  ASSERT_NE(higher_level, nullptr);
  EXPECT_EQ(higher_level->total_qty, 20u);
  EXPECT_EQ(higher_level->order_count, 1u);
  EXPECT_EQ(lower_level->total_qty, 10u);
  EXPECT_EQ(lower_level->order_count, 1u);
  EXPECT_EQ(store.reservePrice(7, 103u).result,
            MutationResult::InvariantViolation);
  EXPECT_EQ(store.counters().committed_pages,
            counters_before.committed_pages);
  EXPECT_EQ(store.counters().page_capacity_failures,
            counters_before.page_capacity_failures);
}

TEST(PriceLevelStoreTest, CapacityFailureDoesNotChangeExistingState) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress existing = preparePrice(store, 7, 10u);
  ASSERT_EQ(store.add(existing, OrderSide::Buy, 100u),
            MutationResult::Applied);

  const auto failed = store.reservePrice(7, 65'536u);
  EXPECT_EQ(failed.result, MutationResult::PricePageCapacityExceeded);
  EXPECT_EQ(store.pageHandle(7, 1), 0u);
  EXPECT_EQ(store.counters().committed_pages, 1u);
  EXPECT_EQ(store.counters().page_capacity_failures, 1u);

  const auto best = store.view(store.best(7, OrderSide::Buy), OrderSide::Buy);
  ASSERT_TRUE(best.valid);
  EXPECT_EQ(best.raw_price, 10u);
  EXPECT_EQ(best.total_qty, 100u);
  EXPECT_EQ(best.order_count, 1u);
}

TEST(PriceLevelStoreTest, RejectsUnpreparedAndInvalidLocate) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  EXPECT_EQ(store.prepareBook(0), MutationResult::InvalidLocate);
  EXPECT_EQ(store.reservePrice(0, 1u).result, MutationResult::InvalidLocate);
  EXPECT_EQ(store.reservePrice(7, 1u).result,
            MutationResult::BookNotPrepared);
}

TEST(PriceLevelStoreTest, BidAndAskAtSamePriceRemainIndependent) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress address = preparePrice(store, 7, 123'456u);

  ASSERT_EQ(store.add(address, OrderSide::Buy, 100u),
            MutationResult::Applied);
  ASSERT_EQ(store.add(address, OrderSide::Sell, 250u),
            MutationResult::Applied);

  const auto bid = store.view(store.best(7, OrderSide::Buy), OrderSide::Buy);
  const auto ask = store.view(store.best(7, OrderSide::Sell), OrderSide::Sell);
  ASSERT_TRUE(bid.valid);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(bid.raw_price, 123'456u);
  EXPECT_EQ(bid.total_qty, 100u);
  EXPECT_EQ(ask.raw_price, 123'456u);
  EXPECT_EQ(ask.total_qty, 250u);

  EXPECT_EQ(store.erase(address, OrderSide::Buy, 100u),
            MutationResult::Applied);
  EXPECT_FALSE(store.best(7, OrderSide::Buy).valid);
  EXPECT_TRUE(store.best(7, OrderSide::Sell).valid);
}

TEST(PriceLevelStoreTest, QuantityAggregationIsUint64) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress address = preparePrice(store, 7, 100u);
  constexpr std::uint32_t max_qty =
      std::numeric_limits<std::uint32_t>::max();

  ASSERT_EQ(store.add(address, OrderSide::Buy, max_qty),
            MutationResult::Applied);
  ASSERT_EQ(store.add(address, OrderSide::Buy, max_qty),
            MutationResult::Applied);
  const auto level = store.view(store.best(7, OrderSide::Buy), OrderSide::Buy);
  ASSERT_TRUE(level.valid);
  EXPECT_EQ(level.total_qty, 8'589'934'590ULL);
  EXPECT_EQ(level.order_count, 2u);

  ASSERT_EQ(store.erase(address, OrderSide::Buy, max_qty),
            MutationResult::Applied);
  EXPECT_TRUE(store.best(7, OrderSide::Buy).valid);
  ASSERT_EQ(store.erase(address, OrderSide::Buy, max_qty),
            MutationResult::Applied);
  EXPECT_FALSE(store.best(7, OrderSide::Buy).valid);
}

TEST(PriceLevelStoreTest, AggregateOverflowChecksDoNotPartiallyMutate) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress address = preparePrice(store, 7, 100u);
  ASSERT_EQ(store.add(address, OrderSide::Buy, 1u),
            MutationResult::Applied);

  PriceLevelState *level = store.resolveLevel(address, OrderSide::Buy);
  ASSERT_NE(level, nullptr);
  level->total_qty = std::numeric_limits<std::uint64_t>::max();
  level->order_count = 1;
  EXPECT_EQ(store.add(address, OrderSide::Buy, 1u),
            MutationResult::QuantityOverflow);
  EXPECT_EQ(level->total_qty, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(level->order_count, 1u);

  level->total_qty = 1;
  level->order_count = std::numeric_limits<std::uint32_t>::max();
  EXPECT_EQ(store.add(address, OrderSide::Buy, 1u),
            MutationResult::OrderCountOverflow);
  EXPECT_EQ(level->total_qty, 1u);
  EXPECT_EQ(level->order_count,
            std::numeric_limits<std::uint32_t>::max());
}

TEST(PriceLevelStoreTest, PriceZeroAndUint32MaxAreBothSearchable) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(65'535u), MutationResult::Applied);
  const PriceAddress minimum_price = preparePrice(store, 65'535u, 0u);
  const PriceAddress maximum_price = preparePrice(
      store, 65'535u, std::numeric_limits<std::uint32_t>::max());

  ASSERT_EQ(store.add(minimum_price, OrderSide::Buy, 1u),
            MutationResult::Applied);
  ASSERT_EQ(store.add(maximum_price, OrderSide::Buy, 2u),
            MutationResult::Applied);
  ASSERT_EQ(store.add(minimum_price, OrderSide::Sell, 3u),
            MutationResult::Applied);
  ASSERT_EQ(store.add(maximum_price, OrderSide::Sell, 4u),
            MutationResult::Applied);

  const auto bid = store.best(65'535u, OrderSide::Buy);
  const auto ask = store.best(65'535u, OrderSide::Sell);
  ASSERT_TRUE(bid.valid);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(bid.raw_price, std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(ask.raw_price, 0u);
}

TEST(PriceLevelStoreTest, RemovingBestFallsBackWithinPageThenAcrossPages) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress p10 = preparePrice(store, 7, 10u);
  const PriceAddress p20 = preparePrice(store, 7, 20u);
  const PriceAddress far = preparePrice(store, 7, 65'536u + 5u);
  ASSERT_EQ(p10.page_handle, p20.page_handle);
  ASSERT_NE(p20.page_handle, far.page_handle);

  ASSERT_EQ(store.add(p10, OrderSide::Buy, 10u), MutationResult::Applied);
  ASSERT_EQ(store.add(p20, OrderSide::Buy, 20u), MutationResult::Applied);
  ASSERT_EQ(store.add(far, OrderSide::Buy, 30u), MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Buy).raw_price, 65'541u);

  ASSERT_EQ(store.erase(far, OrderSide::Buy, 30u),
            MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Buy).raw_price, 20u);
  ASSERT_EQ(store.erase(p20, OrderSide::Buy, 20u),
            MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Buy).raw_price, 10u);
}

TEST(PriceLevelStoreTest, RemovingBestAskUsesSuccessorAcrossBothHierarchies) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress p10 = preparePrice(store, 7, 10u);
  const PriceAddress p20 = preparePrice(store, 7, 20u);
  const PriceAddress far = preparePrice(store, 7, 65'536u + 5u);

  ASSERT_EQ(store.add(p10, OrderSide::Sell, 10u), MutationResult::Applied);
  ASSERT_EQ(store.add(p20, OrderSide::Sell, 20u), MutationResult::Applied);
  ASSERT_EQ(store.add(far, OrderSide::Sell, 30u), MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Sell).raw_price, 10u);

  ASSERT_EQ(store.erase(p10, OrderSide::Sell, 10u),
            MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Sell).raw_price, 20u);
  ASSERT_EQ(store.erase(p20, OrderSide::Sell, 20u),
            MutationResult::Applied);
  EXPECT_EQ(store.best(7, OrderSide::Sell).raw_price, 65'541u);
}

TEST(PriceLevelStoreTest, PartialReductionDoesNotChangeOrderOccupancy) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress address = preparePrice(store, 7, 10u);
  ASSERT_EQ(store.add(address, OrderSide::Buy, 100u),
            MutationResult::Applied);

  ASSERT_EQ(store.reduce(address, OrderSide::Buy, 100u, 40u),
            MutationResult::Applied);
  const PriceLevelState *level = store.resolveLevel(address, OrderSide::Buy);
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->total_qty, 60u);
  EXPECT_EQ(level->order_count, 1u);
  EXPECT_EQ(store.reduce(address, OrderSide::Buy, 60u, 60u),
            MutationResult::QuantityUnderflow);
  EXPECT_TRUE(store.best(7, OrderSide::Buy).valid);
}

TEST(PriceLevelStoreTest,
     CheckedPartialReductionValidatesOwnerBeforeMutation) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  ASSERT_EQ(store.prepareBook(8), MutationResult::Applied);
  const PriceAddress other_book = preparePrice(store, 8, 65'600u);
  ASSERT_EQ(store.add(other_book, OrderSide::Buy, 100u),
            MutationResult::Applied);
  ASSERT_EQ(store.add(other_book, OrderSide::Buy, 200u),
            MutationResult::Applied);

  EXPECT_EQ(store.reduceChecked(7, 1, other_book, OrderSide::Buy, 100u, 25u),
            MutationResult::InvariantViolation);
  const PriceLevelState *level =
      store.resolveLevel(other_book, OrderSide::Buy);
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->total_qty, 300u);
  EXPECT_EQ(level->order_count, 2u);

  EXPECT_EQ(store.reduceChecked(8, 0, other_book, OrderSide::Buy, 100u, 25u),
            MutationResult::InvariantViolation);
  EXPECT_EQ(level->total_qty, 300u);

  ASSERT_EQ(store.reduceChecked(8, 1, other_book, OrderSide::Buy, 100u, 25u),
            MutationResult::Applied);
  EXPECT_EQ(level->total_qty, 275u);
  EXPECT_EQ(level->order_count, 2u);
  EXPECT_EQ(store.reduceChecked(0, 1, other_book, OrderSide::Buy, 100u, 25u),
            MutationResult::InvalidLocate);
  EXPECT_EQ(store.reduceChecked(8, 1, PriceAddress{}, OrderSide::Buy, 100u,
                                25u),
            MutationResult::InvalidPricePage);
  EXPECT_EQ(level->total_qty, 275u);
}

TEST(PriceLevelStoreTest,
     ExistingOrderResolutionUsesHandleAndLevelWithOwnerValidation) {
  static_assert(PriceLevelStore::kExistingOrderRootLookups == 0);
  static_assert(PriceLevelStore::kCachedBestBitmapSearches == 0);
  static_assert(PriceLevelStore::kReservePriceRootSlotReads == 1);
  static_assert(PriceLevelStore::kCommitReservationRootSlotReads == 1);
  static_assert(PriceLevelStore::kCommitReservationRootSlotWrites == 1);
  static_assert(PriceLevelStore::kNewPriceTransactionRootSlotReads == 2);
  static_assert(PriceLevelStore::kMaxNextWorseBitmapSearches == 2);

  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(65'535u), MutationResult::Applied);
  const PriceAddress address = preparePrice(store, 65'535u, 0xffff'ffffu);
  ASSERT_EQ(store.add(address, OrderSide::Sell, 55u),
            MutationResult::Applied);

  const PriceLevelState *first =
      store.resolveLevel(address, OrderSide::Sell);
  const PriceLevelState *second =
      store.resolveLevel(address, OrderSide::Sell);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first->total_qty, 55u);
}

TEST(PriceLevelStoreTest, NextWorseAndTopTenCrossPageBoundaries) {
  static_assert(PriceLevelStore::kMaxTopTenNextWorseCalls == 9);
  static_assert(PriceLevelStore::kMaxTopTenBitmapSearches == 18);

  PriceLevelStore store(PriceLevelStoreConfig{12, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  std::array<PriceAddress, 12> addresses{};
  for (std::uint32_t i = 0; i < 12; ++i) {
    const std::uint32_t price = (i << 16) | (i * 17u);
    addresses[i] = preparePrice(store, 7, price);
    ASSERT_EQ(store.add(addresses[i], OrderSide::Buy, i + 1),
              MutationResult::Applied);
  }

  std::array<astra::book::PriceLevelView, 10> levels{};
  ASSERT_EQ(store.topTen(7, OrderSide::Buy, levels), 10u);
  for (std::uint32_t rank = 0; rank < 10; ++rank) {
    const std::uint32_t i = 11u - rank;
    EXPECT_TRUE(levels[rank].valid);
    EXPECT_EQ(levels[rank].raw_price, (i << 16) | (i * 17u));
    EXPECT_EQ(levels[rank].total_qty, i + 1u);
  }

  auto cursor = store.best(7, OrderSide::Buy);
  for (std::uint32_t rank = 0; rank < 12; ++rank) {
    const std::uint32_t i = 11u - rank;
    const auto level = store.view(cursor, OrderSide::Buy);
    ASSERT_TRUE(level.valid);
    EXPECT_EQ(level.raw_price, (i << 16) | (i * 17u));
    cursor = store.nextWorse(7, OrderSide::Buy, cursor);
  }
  EXPECT_FALSE(cursor.valid);

  for (std::uint32_t i = 11; i >= 2; --i) {
    ASSERT_EQ(store.erase(addresses[i], OrderSide::Buy, i + 1u),
              MutationResult::Applied);
  }
  levels = {};
  ASSERT_EQ(store.topTen(7, OrderSide::Buy, levels), 2u);
  EXPECT_EQ(levels[0].raw_price, (1u << 16) | 17u);
  EXPECT_EQ(levels[0].total_qty, 2u);
  EXPECT_EQ(levels[1].raw_price, 0u);
  EXPECT_EQ(levels[1].total_qty, 1u);
}

TEST(PriceLevelStoreTest, MoveIsTransactionalForLogicalState) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress old_address = preparePrice(store, 7, 10u);
  const PriceAddress new_address = preparePrice(store, 7, 65'600u);
  ASSERT_EQ(store.add(old_address, OrderSide::Sell, 100u),
            MutationResult::Applied);

  ASSERT_EQ(store.move(old_address, 100u, new_address, 75u,
                       OrderSide::Sell),
            MutationResult::Applied);
  const auto best = store.view(store.best(7, OrderSide::Sell),
                               OrderSide::Sell);
  ASSERT_TRUE(best.valid);
  EXPECT_EQ(best.raw_price, 65'600u);
  EXPECT_EQ(best.total_qty, 75u);
  EXPECT_EQ(store.resolveLevel(old_address, OrderSide::Sell)->order_count, 0u);
}

TEST(PriceLevelStoreTest, MoveCannotCrossStockLocate) {
  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  ASSERT_EQ(store.prepareBook(8), MutationResult::Applied);
  const PriceAddress old_address = preparePrice(store, 7, 10u);
  const PriceAddress other_book = preparePrice(store, 8, 20u);
  ASSERT_EQ(store.add(old_address, OrderSide::Buy, 100u),
            MutationResult::Applied);

  EXPECT_EQ(store.move(old_address, 100u, other_book, 75u, OrderSide::Buy),
            MutationResult::LocateMismatch);
  const auto best = store.view(store.best(7, OrderSide::Buy), OrderSide::Buy);
  ASSERT_TRUE(best.valid);
  EXPECT_EQ(best.raw_price, 10u);
  EXPECT_EQ(best.total_qty, 100u);
  EXPECT_FALSE(store.best(8, OrderSide::Buy).valid);
}

TEST(PriceLevelStoreTest,
     MoveDestinationPreflightLeavesSourceUntouchedOnMetadataFailure) {
  using astra::book::PriceLevelStoreTestPeer;

  PriceLevelStore store(PriceLevelStoreConfig{2, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress source = preparePrice(store, 7, 10u);
  const PriceAddress destination = preparePrice(store, 7, 65'600u);
  ASSERT_EQ(store.add(source, OrderSide::Buy, 100u),
            MutationResult::Applied);
  PriceLevelStoreTestPeer::setPageSummary(
      store, destination.page_handle, OrderSide::Buy, 0,
      astra::book::kPricePartCount);

  EXPECT_EQ(store.move(source, 100u, destination, 75u, OrderSide::Buy),
            MutationResult::InvariantViolation);
  const PriceLevelState *source_level =
      store.resolveLevel(source, OrderSide::Buy);
  const PriceLevelState *destination_level =
      store.resolveLevel(destination, OrderSide::Buy);
  ASSERT_NE(source_level, nullptr);
  ASSERT_NE(destination_level, nullptr);
  EXPECT_EQ(source_level->total_qty, 100u);
  EXPECT_EQ(source_level->order_count, 1u);
  EXPECT_EQ(destination_level->total_qty, 0u);
  EXPECT_EQ(destination_level->order_count, 0u);
  const auto best = store.best(7, OrderSide::Buy);
  ASSERT_TRUE(best.valid);
  EXPECT_EQ(best.raw_price, 10u);
}

TEST(PriceLevelStoreTest, DestinationCapacityErrorLeavesSourceUnchanged) {
  PriceLevelStore store(PriceLevelStoreConfig{1, false});
  ASSERT_EQ(store.prepareBook(7), MutationResult::Applied);
  const PriceAddress old_address = preparePrice(store, 7, 10u);
  ASSERT_EQ(store.add(old_address, OrderSide::Buy, 100u),
            MutationResult::Applied);

  const auto destination = store.reservePrice(7, 65'536u);
  ASSERT_EQ(destination.result,
            MutationResult::PricePageCapacityExceeded);
  const auto best = store.view(store.best(7, OrderSide::Buy), OrderSide::Buy);
  ASSERT_TRUE(best.valid);
  EXPECT_EQ(best.raw_price, 10u);
  EXPECT_EQ(best.total_qty, 100u);
  EXPECT_EQ(best.order_count, 1u);
}
