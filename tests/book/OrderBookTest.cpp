#include "astra/book/OrderBook.hpp"
#include "astra/book/TopOfBook.hpp"

#include <gtest/gtest.h>

#include <limits>

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
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.cancelShares(1, 60); // 100 - 60 = 40 remaining

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   40u);
}

TEST(OrderBookTest, DeleteOrderFallsBackToNextBestBid) {
  OrderBook book(42);
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
  OrderBook book(42);
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
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.replaceOrder(1, 2, 12, 75); // old id=1 → new id=2 at price 12 qty 75

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 12u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(OrderBookTest, SameReferenceReplaceChangesQuantityAtSamePrice) {
  OrderBook book(42);
  ASSERT_EQ(book.addOrder(1, 10, 100, 'B'),
            astra::book::MutationResult::Applied);

  ASSERT_EQ(book.replaceOrder(1, 1, 10, 75),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(book.liveOrderCount(), 1u);
  EXPECT_EQ(book.getTopOfBook().bid_price, 10u);
  EXPECT_EQ(book.getTopOfBook().bid_qty, 75u);
  EXPECT_EQ(book.executeOrder(1, 75),
            astra::book::MutationResult::Applied);
}

TEST(OrderBookTest, SameReferenceReplaceMovesAcrossPricePages) {
  OrderBook book(42);
  ASSERT_EQ(book.addOrder(1, 10, 100, 'S'),
            astra::book::MutationResult::Applied);

  ASSERT_EQ(book.replaceOrder(1, 1, 65'600, 75),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(book.liveOrderCount(), 1u);
  EXPECT_EQ(book.getTopOfBook().ask_price, 65'600u);
  EXPECT_EQ(book.getTopOfBook().ask_qty, 75u);
  EXPECT_EQ(book.executeOrder(1, 75),
            astra::book::MutationResult::Applied);
}

TEST(OrderBookTest, FullCancelIsRejectedBecauseDeleteRemovesFinalOrder) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');
  EXPECT_EQ(book.cancelShares(1, 100),
            astra::book::MutationResult::QuantityExceeded);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty, 100u);
}

TEST(OrderBookTest, HandlesSparseLargeOrderIdWithoutDenseAllocation) {
  constexpr uint64_t large_id = 9'000'000'000'000'000'000ULL;
  OrderBook book(42);

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
  OrderBook book(42);
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
  OrderBook book(42);
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
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');
  EXPECT_TRUE(book.trade(1, 25));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,   75u);
}

TEST(OrderBookTest, FullTradeFallsBackToNextBestBid) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 11,  50, 'B');
  EXPECT_TRUE(book.trade(2, 50));

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, TradeLargerThanOrderQuantityIsIgnored) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');
  book.addOrder(2, 10, 100, 'B');
  EXPECT_FALSE(book.trade(1, 150)); // exceeds order qty — ignored

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  200u);
}

TEST(OrderBookTest, DuplicateOrderIdIsIgnored) {
  OrderBook book(42);
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
  OrderBook book(42);
  book.addOrder(1, 10, 0, 'B');

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty,   0u);
}

TEST(OrderBookTest, MissingOrderOperationsDoNotChangeBook) {
  OrderBook book(42);
  book.addOrder(1, 10, 100, 'B');

  book.cancelShares(99, 50);   // unknown id — ignored
  book.deleteOrder(99);        // unknown id — ignored
  EXPECT_FALSE(book.trade(99, 50)); // unknown id — ignored

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 10u);
  EXPECT_EQ(top.bid_qty,  100u);
}

TEST(OrderBookTest, BookUpdateCapsAtTopTenLevels) {
  OrderBook book(42);
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

TEST(OrderBookTest, CoversPriceZeroAndUint32MaxWithoutRecentering) {
  OrderBook book(42);
  constexpr uint32_t max_price = std::numeric_limits<uint32_t>::max();

  ASSERT_EQ(book.addOrder(1, 0, 100, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(book.addOrder(2, max_price, 200, 'S'),
            astra::book::MutationResult::Applied);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_TRUE(top.has_bid);
  EXPECT_EQ(top.bid_price, 0u);
  EXPECT_EQ(top.bid_qty, 100u);
  EXPECT_TRUE(top.has_ask);
  EXPECT_EQ(top.ask_price, max_price);
  EXPECT_EQ(top.ask_qty, 200u);
}

TEST(OrderBookTest, HandlesPricesMillionsOfTicksApartOnOneSide) {
  OrderBook book(42);
  ASSERT_EQ(book.addOrder(1, 1, 10, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(book.addOrder(2, 1'999'999'900u, 20, 'B'),
            astra::book::MutationResult::Applied);

  BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 2u);
  EXPECT_EQ(update.bids[0].price, 1'999'999'900u);
  EXPECT_EQ(update.bids[1].price, 1u);

  ASSERT_EQ(book.deleteOrder(2), astra::book::MutationResult::Applied);
  EXPECT_EQ(book.getTopOfBook().bid_price, 1u);
}

TEST(OrderBookTest, StandaloneCapacitySupportsMoreThan256DistinctPricePages) {
  constexpr std::uint32_t page_count = 257;
  OrderBook book(42, 300);

  for (std::uint32_t page_index = 0; page_index < page_count; ++page_index) {
    ASSERT_EQ(book.addOrder(static_cast<std::uint64_t>(page_index) + 1,
                            page_index << 16, 1, 'B'),
              astra::book::MutationResult::Applied);
  }

  EXPECT_EQ(book.liveOrderCount(), page_count);
  EXPECT_EQ(book.getTopOfBook().bid_price, (page_count - 1) << 16);
}

TEST(OrderBookTest, BidAndAskAtSameRawPriceAreIndependent) {
  OrderBook book(42);
  ASSERT_EQ(book.addOrder(1, 123'456, 100, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(book.addOrder(2, 123'456, 250, 'S'),
            astra::book::MutationResult::Applied);

  const TopOfBook top = book.getTopOfBook();
  EXPECT_EQ(top.bid_price, 123'456u);
  EXPECT_EQ(top.bid_qty, 100u);
  EXPECT_EQ(top.ask_price, 123'456u);
  EXPECT_EQ(top.ask_qty, 250u);

  ASSERT_EQ(book.deleteOrder(1), astra::book::MutationResult::Applied);
  EXPECT_FALSE(book.getTopOfBook().has_bid);
  EXPECT_TRUE(book.getTopOfBook().has_ask);
}

TEST(OrderBookTest, AggregateQuantityDoesNotTruncateAtUint32Max) {
  OrderBook book(42);
  constexpr uint32_t max_qty = std::numeric_limits<uint32_t>::max();
  ASSERT_EQ(book.addOrder(1, 100, max_qty, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(book.addOrder(2, 100, max_qty, 'B'),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(book.getTopOfBook().bid_qty, 8'589'934'590ULL);
  const BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 1u);
  EXPECT_EQ(update.bids[0].qty, 8'589'934'590ULL);
  EXPECT_EQ(update.bids[0].num_orders, 2u);
}

TEST(OrderBookTest, DuplicateReplacementIdLeavesOldOrderUnchanged) {
  OrderBook book(42);
  ASSERT_EQ(book.addOrder(1, 10, 100, 'B'),
            astra::book::MutationResult::Applied);
  ASSERT_EQ(book.addOrder(2, 20, 200, 'B'),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(book.replaceOrder(1, 2, 1'000'000, 50),
            astra::book::MutationResult::DuplicateOrderRef);

  const BookUpdate update = book.getBookUpdate();
  ASSERT_EQ(update.bids_depth, 2u);
  EXPECT_EQ(update.bids[0].price, 20u);
  EXPECT_EQ(update.bids[0].qty, 200u);
  EXPECT_EQ(update.bids[1].price, 10u);
  EXPECT_EQ(update.bids[1].qty, 100u);
  EXPECT_EQ(book.executeOrder(1, 25), astra::book::MutationResult::Applied);
  EXPECT_EQ(book.getBookUpdate().bids[1].qty, 75u);
}

TEST(OrderBookTest, RejectsPriceBeyondWireDomainWithoutChangingBook) {
  OrderBook book(42);
  ASSERT_EQ(book.addOrder(1, 10, 100, 'B'),
            astra::book::MutationResult::Applied);

  EXPECT_EQ(book.addOrder(2, std::uint64_t{1} << 32, 50, 'B'),
            astra::book::MutationResult::PriceOutOfRange);
  EXPECT_EQ(book.replaceOrder(1, 3, std::uint64_t{1} << 32, 25),
            astra::book::MutationResult::PriceOutOfRange);
  EXPECT_EQ(book.getTopOfBook().bid_price, 10u);
  EXPECT_EQ(book.getTopOfBook().bid_qty, 100u);
}
