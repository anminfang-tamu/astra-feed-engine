#include "astra/book/BookManager.hpp"
#include "astra/book/BookUpdate.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <gtest/gtest.h>

#include <limits>

TEST(BookManagerTest, UnknownLocateReturnsNull) {
  BookManager manager;
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, RoutesOrdersByStockLocate) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(7), nullptr);
  ASSERT_NE(manager.getOrCreate(9), nullptr);
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
  BookManager manager;
  EXPECT_EQ(manager.deleteOrder(42, 999),
            astra::book::MutationResult::BookNotPrepared);
  EXPECT_EQ(manager.getOrderBook(42), nullptr);
}

TEST(BookManagerTest, DeleteRemovesOrder) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(1), nullptr);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.deleteOrder(1, 10);

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(BookManagerTest, CancelSharesReducesQuantity) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(1), nullptr);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.cancelShares(1, 10, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty,   40u);
  const astra::book::OrderState *state =
      manager.orderTable().findState(10);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->qty, 40u);
}

TEST(BookManagerTest, TradeReducesQuantity) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(1), nullptr);
  manager.addOrder(1, 10, 50, 100, 'B');
  EXPECT_TRUE(manager.trade(1, 10, 30));

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 50u);
  EXPECT_EQ(top.bid_qty,   70u);
}

TEST(BookManagerTest, ReplaceOrderUpdatesBook) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(1), nullptr);
  manager.addOrder(1, 10, 50, 100, 'B');
  manager.replaceOrder(1, 10, 20, 60, 75); // old=10 → new=20 at price 60 qty 75

  const TopOfBook top = manager.getOrderBook(1)->getTopOfBook();
  EXPECT_EQ(top.bid_price, 60u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(BookManagerTest, ZeroLocateIsIgnored) {
  BookManager manager;
  manager.addOrder(0, 1, 100, 50, 'B'); // locate 0 is invalid
  EXPECT_EQ(manager.getOrderBook(0), nullptr);
}

TEST(BookManagerTest, HotAddDoesNotAllocateAnUnpreparedBook) {
  BookManager manager;
  EXPECT_EQ(manager.addOrder(7, 1, 100, 10, 'B'),
            astra::book::MutationResult::BookNotPrepared);
  EXPECT_EQ(manager.getOrderBook(7), nullptr);
}

TEST(BookManagerTest, GetOrCreateUsesConfiguredGlobalCapacity) {
  const BookManagerConfig config{{128, 8, false}, {16, false}};
  BookManager manager(config);

  (void)manager.getOrCreate(7);

  const OrderBook *book = manager.getOrderBook(7);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->orderCapacity(), 128u);
  EXPECT_EQ(book->liveOrderCount(), 0u);
}

TEST(BookManagerTest, EnforcesGlobalOrderReferenceUniqueness) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(7), nullptr);
  ASSERT_NE(manager.getOrCreate(8), nullptr);

  EXPECT_EQ(manager.addOrder(7, 1, 100, 10, 'B'),
            astra::book::MutationResult::Applied);
  EXPECT_EQ(manager.addOrder(8, 1, 200, 20, 'S'),
            astra::book::MutationResult::DuplicateOrderRef);

  EXPECT_EQ(manager.getOrderBook(7)->getTopOfBook().bid_qty, 10u);
  EXPECT_FALSE(manager.getOrderBook(8)->getTopOfBook().has_ask);
}

TEST(BookManagerTest, WrongLocateMutationCannotTouchOwningBook) {
  BookManager manager;
  ASSERT_NE(manager.getOrCreate(7), nullptr);
  ASSERT_NE(manager.getOrCreate(8), nullptr);
  ASSERT_EQ(manager.addOrder(7, 99, 100, 50, 'B'),
            astra::book::MutationResult::Applied);
  astra::book::OrderState *const resident =
      manager.orderTable().findState(99);
  ASSERT_NE(resident, nullptr);
  EXPECT_EQ(resident->locate, 7u);

  EXPECT_EQ(manager.executeOrder(8, 99, 10),
            astra::book::MutationResult::LocateMismatch);
  EXPECT_EQ(manager.getOrderBook(7)->getTopOfBook().bid_qty, 50u);
  EXPECT_FALSE(manager.getOrderBook(8)->getTopOfBook().has_bid);
}

TEST(BookManagerTest, FallbackHotLookupExecutesAndMisses) {
  BookManager manager(BookManagerConfig{{2, 4, false}, {2, false}});
  OrderBook *const book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  constexpr std::uint64_t ref = std::numeric_limits<std::uint64_t>::max();
  ASSERT_EQ(manager.addOrder(7, ref, 100, 100, 'B'),
            astra::book::MutationResult::Applied);

  const auto diagnostic = manager.orderTable().find(ref);
  ASSERT_TRUE(diagnostic.found());
  ASSERT_NE(diagnostic.path, astra::book::LookupPath::Direct);
  ASSERT_EQ(manager.orderTable().findState(ref), diagnostic.state);

  EXPECT_EQ(manager.executeOrder(7, ref, 25),
            astra::book::MutationResult::Applied);
  EXPECT_EQ(diagnostic.state->qty, 75u);
  EXPECT_EQ(book->getTopOfBook().bid_qty, 75u);

  constexpr std::uint64_t missing_ref = ref - 1;
  EXPECT_EQ(manager.orderTable().findState(missing_ref), nullptr);
  EXPECT_EQ(manager.executeOrder(7, missing_ref, 1),
            astra::book::MutationResult::OrderNotFound);
  EXPECT_FALSE(book->localInvalid());
}

TEST(BookManagerTest, PricePageExhaustionLeavesReplacementSourceIntact) {
  const BookManagerConfig config{{16, 2, false}, {1, false}};
  BookManager manager(config);
  OrderBook *book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(manager.addOrder(7, 1, 10, 100, 'B'),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(manager.replaceOrder(7, 1, 2, 65'536, 50),
            astra::book::MutationResult::PricePageCapacityExceeded);
  EXPECT_EQ(book->getTopOfBook().bid_price, 10u);
  EXPECT_EQ(book->getTopOfBook().bid_qty, 100u);
  EXPECT_EQ(book->liveOrderCount(), 1u);
  EXPECT_EQ(manager.executeOrder(7, 1, 25),
            astra::book::MutationResult::Applied);
  EXPECT_EQ(book->getTopOfBook().bid_qty, 75u);
  EXPECT_EQ(manager.executeOrder(7, 2, 1),
            astra::book::MutationResult::OrderNotFound);
}

TEST(BookManagerTest, OrderTableExhaustionLeavesReplacementSourceIntact) {
  // refs 0-1 are direct.  One fallback bucket has exactly two fixed ways.
  const BookManagerConfig config{{2, 1, false}, {2, false}};
  BookManager manager(config);
  OrderBook *book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(manager.addOrder(7, 1, 10, 100, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(manager.addOrder(7, 2, 20, 10, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(manager.addOrder(7, 3, 30, 10, 'B'),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(manager.replaceOrder(7, 1, 4, 40, 50),
            astra::book::MutationResult::OrderTableCapacityExceeded);

  EXPECT_EQ(book->liveOrderCount(), 3u);
  ASSERT_EQ(manager.executeOrder(7, 1, 25),
            astra::book::MutationResult::Applied);
  const BookUpdate update = book->getBookUpdate();
  ASSERT_EQ(update.bids_depth, 3u);
  EXPECT_EQ(update.bids[2].price, 10u);
  EXPECT_EQ(update.bids[2].qty, 75u);
}

TEST(BookManagerTest, OrderStateStoresDirectPriceAddressForExistingMutation) {
  BookManager manager(BookManagerConfig{{128, 2, false}, {4, false}});
  ASSERT_NE(manager.getOrCreate(7), nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 0x1234'abcd, 100, 'S'),
            astra::book::MutationResult::Applied);

  const auto lookup = manager.orderTable().find(55);
  ASSERT_TRUE(lookup.found());
  EXPECT_NE(lookup.state->price_page_handle, 0u);
  EXPECT_EQ(lookup.state->price_level_index, 0xabcdu);
  EXPECT_EQ(lookup.state->price_page_index, 0x1234u);
  EXPECT_EQ(lookup.state->locate, 7u);
  EXPECT_EQ(lookup.state->side,
            static_cast<std::uint8_t>(OrderSide::Sell));

  const auto pages_before = manager.priceLevels().counters().committed_pages;
  ASSERT_EQ(manager.executeOrder(7, 55, 25),
            astra::book::MutationResult::Applied);
  EXPECT_EQ(manager.priceLevels().counters().committed_pages, pages_before);
  EXPECT_EQ(manager.getOrderBook(7)->getTopOfBook().ask_qty, 75u);

  ASSERT_EQ(manager.replaceOrder(7, 55, 55, 0xabcd'0001u, 60u),
            astra::book::MutationResult::Applied);
  EXPECT_EQ(lookup.state->price_page_index, 0xabcdu);
  EXPECT_EQ(lookup.state->price_level_index, 1u);
  ASSERT_EQ(manager.executeOrder(7, 55, 10u),
            astra::book::MutationResult::Applied);
  EXPECT_EQ(manager.getOrderBook(7)->getTopOfBook().ask_qty, 50u);
}

TEST(BookManagerTest, CorruptOrderStateMakesBookStickyInvalid) {
  BookManager manager(BookManagerConfig{{128, 2, false}, {4, false}});
  OrderBook *book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 100, 100, 'B'),
            astra::book::MutationResult::Applied);

  astra::book::OrderState *const state =
      manager.orderTable().findState(55);
  ASSERT_NE(state, nullptr);
  state->price_page_handle = astra::book::kInvalidPricePageHandle;

  EXPECT_EQ(manager.executeOrder(7, 55, 25),
            astra::book::MutationResult::InvariantViolation);
  EXPECT_TRUE(book->localInvalid());
  EXPECT_EQ(manager.addOrder(7, 56, 200, 10, 'S'),
            astra::book::MutationResult::BookInvalid);
  EXPECT_EQ(book->liveOrderCount(), 1u);
  EXPECT_FALSE(book->getTopOfBook().has_ask);
}

TEST(BookManagerTest, AggregateUnderflowRollsBackReplacementAndSealsBook) {
  BookManager manager(BookManagerConfig{{128, 4, false}, {4, false}});
  OrderBook *book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 100, 100, 'B'),
            astra::book::MutationResult::Applied);

  const auto old_lookup = manager.orderTable().find(55);
  ASSERT_TRUE(old_lookup.found());
  const astra::book::PriceAddress old_address{
      old_lookup.state->price_page_handle,
      old_lookup.state->price_level_index, 0};
  astra::book::PriceLevelState *level =
      manager.priceLevels().resolveLevel(old_address, OrderSide::Buy);
  ASSERT_NE(level, nullptr);
  level->total_qty = 50; // Controlled corruption: OrderState still says 100.

  EXPECT_EQ(manager.replaceOrder(7, 55, 56, 65'600, 75),
            astra::book::MutationResult::QuantityUnderflow);

  EXPECT_TRUE(manager.orderTable().find(55).found());
  EXPECT_FALSE(manager.orderTable().find(56).found());
  EXPECT_EQ(manager.orderTable().find(55).state->qty, 100u);
  EXPECT_EQ(level->total_qty, 50u);
  EXPECT_EQ(level->order_count, 1u);
  EXPECT_EQ(book->liveOrderCount(), 1u);
  EXPECT_TRUE(book->localInvalid());
  EXPECT_EQ(manager.executeOrder(7, 55, 1),
            astra::book::MutationResult::BookInvalid);
}

TEST(BookManagerTest, PartialExecutionDetectsAggregateBelowWholeOrder) {
  BookManager manager(BookManagerConfig{{128, 4, false}, {4, false}});
  OrderBook *book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 100, 100, 'B'),
            astra::book::MutationResult::Applied);

  const auto lookup = manager.orderTable().find(55);
  ASSERT_TRUE(lookup.found());
  const astra::book::PriceAddress address{
      lookup.state->price_page_handle, lookup.state->price_level_index, 0};
  astra::book::PriceLevelState *level =
      manager.priceLevels().resolveLevel(address, OrderSide::Buy);
  ASSERT_NE(level, nullptr);
  level->total_qty = 50; // Controlled corruption: OrderState still says 100.

  EXPECT_EQ(manager.executeOrder(7, 55, 25),
            astra::book::MutationResult::QuantityUnderflow);
  EXPECT_EQ(lookup.state->qty, 100u);
  EXPECT_EQ(level->total_qty, 50u);
  EXPECT_TRUE(book->localInvalid());
}

TEST(BookManagerTest, ImpossibleAggregateOverflowSealsBook) {
  BookManager manager(BookManagerConfig{{128, 4, false}, {4, false}});
  OrderBook *book = manager.getOrCreate(7);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 100, 1, 'S'),
            astra::book::MutationResult::Applied);

  const auto lookup = manager.orderTable().find(55);
  ASSERT_TRUE(lookup.found());
  const astra::book::PriceAddress address{
      lookup.state->price_page_handle, lookup.state->price_level_index, 0};
  astra::book::PriceLevelState *level =
      manager.priceLevels().resolveLevel(address, OrderSide::Sell);
  ASSERT_NE(level, nullptr);
  level->total_qty = std::numeric_limits<std::uint64_t>::max();

  EXPECT_EQ(manager.addOrder(7, 56, 100, 1, 'S'),
            astra::book::MutationResult::QuantityOverflow);
  EXPECT_FALSE(manager.orderTable().find(56).found());
  EXPECT_TRUE(book->localInvalid());
}

TEST(BookManagerTest,
     CrossBookPageHandleCorruptionOnPartialExecuteSealsOwnerBook) {
  BookManager manager(BookManagerConfig{{128, 4, false}, {4, false}});
  OrderBook *book7 = manager.getOrCreate(7);
  OrderBook *book8 = manager.getOrCreate(8);
  ASSERT_NE(book7, nullptr);
  ASSERT_NE(book8, nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 100, 100, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(manager.addOrder(8, 56, 65'600, 200, 'B'),
            astra::book::MutationResult::Applied);

  const auto owner = manager.orderTable().find(56);
  const auto corrupted = manager.orderTable().find(55);
  ASSERT_TRUE(owner.found());
  ASSERT_TRUE(corrupted.found());
  corrupted.state->price_page_handle = owner.state->price_page_handle;
  corrupted.state->price_level_index = owner.state->price_level_index;

  EXPECT_EQ(manager.executeOrder(7, 55, 25),
            astra::book::MutationResult::InvariantViolation);
  EXPECT_EQ(corrupted.state->qty, 100u);
  EXPECT_EQ(book8->getTopOfBook().bid_qty, 200u);
  EXPECT_TRUE(book7->localInvalid());
  EXPECT_EQ(manager.executeOrder(7, 55, 1),
            astra::book::MutationResult::BookInvalid);
}

TEST(BookManagerTest,
     CrossBookPageHandleCorruptionOnCancelSealsOwnerBook) {
  BookManager manager(BookManagerConfig{{128, 4, false}, {4, false}});
  OrderBook *book7 = manager.getOrCreate(7);
  OrderBook *book8 = manager.getOrCreate(8);
  ASSERT_NE(book7, nullptr);
  ASSERT_NE(book8, nullptr);
  ASSERT_EQ(manager.addOrder(7, 55, 100, 100, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(manager.addOrder(8, 56, 65'600, 200, 'B'),
            astra::book::MutationResult::Applied);

  const auto owner = manager.orderTable().find(56);
  const auto corrupted = manager.orderTable().find(55);
  ASSERT_TRUE(owner.found());
  ASSERT_TRUE(corrupted.found());
  corrupted.state->price_page_handle = owner.state->price_page_handle;
  corrupted.state->price_level_index = owner.state->price_level_index;

  EXPECT_EQ(manager.cancelShares(7, 55, 25),
            astra::book::MutationResult::InvariantViolation);
  EXPECT_EQ(corrupted.state->qty, 100u);
  EXPECT_EQ(book8->getTopOfBook().bid_qty, 200u);
  EXPECT_TRUE(book7->localInvalid());
  EXPECT_EQ(manager.cancelShares(7, 55, 1),
            astra::book::MutationResult::BookInvalid);
}

namespace {

class SameBookWrongPageHandleTest : public ::testing::Test {
protected:
  void SetUp() override {
    book_ = manager_.getOrCreate(kLocate);
    ASSERT_NE(book_, nullptr);
    ASSERT_EQ(manager_.addOrder(kLocate, kOriginalRef, 100, 100, 'B'),
              astra::book::MutationResult::Applied);
    ASSERT_EQ(manager_.addOrder(kLocate, kSubstituteRef, 65'636, 200, 'B'),
              astra::book::MutationResult::Applied);

    original_ = manager_.orderTable().findState(kOriginalRef);
    substitute_ = manager_.orderTable().findState(kSubstituteRef);
    ASSERT_NE(original_, nullptr);
    ASSERT_NE(substitute_, nullptr);
    ASSERT_EQ(original_->price_level_index, 100u);
    ASSERT_EQ(substitute_->price_level_index, 100u);
    ASSERT_EQ(original_->price_page_index, 0u);
    ASSERT_EQ(substitute_->price_page_index, 1u);
    ASSERT_NE(original_->price_page_handle, substitute_->price_page_handle);

    original_level_ = manager_.priceLevels().resolveLevel(
        {original_->price_page_handle, original_->price_level_index, 0},
        OrderSide::Buy);
    substitute_level_ = manager_.priceLevels().resolveLevel(
        {substitute_->price_page_handle, substitute_->price_level_index, 0},
        OrderSide::Buy);
    ASSERT_NE(original_level_, nullptr);
    ASSERT_NE(substitute_level_, nullptr);

    // Corrupt only the handle.  The identical low 16-bit price part made this
    // same-stock page substitution indistinguishable before page_index existed.
    original_->price_page_handle = substitute_->price_page_handle;
  }

  void expectBothOrdersAndLevelsUnchanged() {
    EXPECT_EQ(original_->qty, 100u);
    EXPECT_EQ(substitute_->qty, 200u);
    EXPECT_EQ(original_level_->total_qty, 100u);
    EXPECT_EQ(original_level_->order_count, 1u);
    EXPECT_EQ(substitute_level_->total_qty, 200u);
    EXPECT_EQ(substitute_level_->order_count, 1u);
    EXPECT_EQ(book_->liveOrderCount(), 2u);
    EXPECT_NE(manager_.orderTable().findState(kOriginalRef), nullptr);
    EXPECT_NE(manager_.orderTable().findState(kSubstituteRef), nullptr);
    EXPECT_TRUE(book_->localInvalid());
  }

  static constexpr std::uint16_t kLocate = 7;
  static constexpr std::uint64_t kOriginalRef = 55;
  static constexpr std::uint64_t kSubstituteRef = 56;

  BookManager manager_{BookManagerConfig{{128, 4, false}, {4, false}}};
  OrderBook *book_{nullptr};
  astra::book::OrderState *original_{nullptr};
  astra::book::OrderState *substitute_{nullptr};
  astra::book::PriceLevelState *original_level_{nullptr};
  astra::book::PriceLevelState *substitute_level_{nullptr};
};

} // namespace

TEST_F(SameBookWrongPageHandleTest, PartialExecutionFailsBeforeMutation) {
  EXPECT_EQ(manager_.executeOrder(kLocate, kOriginalRef, 25),
            astra::book::MutationResult::InvariantViolation);
  expectBothOrdersAndLevelsUnchanged();
}

TEST_F(SameBookWrongPageHandleTest, PartialCancelFailsBeforeMutation) {
  EXPECT_EQ(manager_.cancelShares(kLocate, kOriginalRef, 25),
            astra::book::MutationResult::InvariantViolation);
  expectBothOrdersAndLevelsUnchanged();
}

TEST_F(SameBookWrongPageHandleTest, FullExecutionFailsBeforeMutation) {
  EXPECT_EQ(manager_.executeOrder(kLocate, kOriginalRef, 100),
            astra::book::MutationResult::InvariantViolation);
  expectBothOrdersAndLevelsUnchanged();
}

TEST_F(SameBookWrongPageHandleTest, ReplaceFailsBeforeMutation) {
  EXPECT_EQ(manager_.replaceOrder(kLocate, kOriginalRef, 57, 200, 75),
            astra::book::MutationResult::InvariantViolation);
  expectBothOrdersAndLevelsUnchanged();
  EXPECT_EQ(manager_.orderTable().findState(57), nullptr);
}
