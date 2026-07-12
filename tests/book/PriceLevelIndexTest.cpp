#include "astra/book/PriceLevelIndex.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace {

using Side = PriceLevelIndex::Side;
using EnsureStatus = PriceLevelIndex::EnsureStatus;
using EraseStatus = PriceLevelIndex::EraseStatus;

PriceLevelArena makeArena(size_t nodes = 128, size_t leaves = 64,
                          size_t levels = 128) {
  return PriceLevelArena({nodes, leaves, levels});
}

} // namespace

TEST(PriceLevelIndexTest, CoversCompleteUint32DomainAndKeepsSidesDistinct) {
  auto arena = makeArena();
  PriceLevelIndex index(arena);

  const auto zero_bid = index.ensure(0, Side::Bid);
  const auto max_bid =
      index.ensure(std::numeric_limits<uint32_t>::max(), Side::Bid);
  const auto zero_ask = index.ensure(0, Side::Ask);
  const auto max_ask =
      index.ensure(std::numeric_limits<uint32_t>::max(), Side::Ask);

  ASSERT_TRUE(zero_bid.ok());
  ASSERT_TRUE(max_bid.ok());
  ASSERT_TRUE(zero_ask.ok());
  ASSERT_TRUE(max_ask.ok());
  EXPECT_NE(zero_bid.handle, zero_ask.handle);
  EXPECT_NE(max_bid.handle, max_ask.handle);
  EXPECT_EQ(index.find(0, Side::Bid), zero_bid.handle);
  EXPECT_EQ(index.find(0, Side::Ask), zero_ask.handle);
  EXPECT_EQ(index.find(std::numeric_limits<uint32_t>::max(), Side::Bid),
            max_bid.handle);
  EXPECT_EQ(index.find(std::numeric_limits<uint32_t>::max(), Side::Ask),
            max_ask.handle);

  PooledPriceLevel *level = arena.level(max_bid.handle);
  ASSERT_NE(level, nullptr);
  level->total_qty = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) +
                     1234u;
  EXPECT_GT(level->total_qty,
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
}

TEST(PriceLevelIndexTest, FindsBestAndTraversesAcrossEveryByteBoundary) {
  auto arena = makeArena(256, 128, 256);
  PriceLevelIndex index(arena);
  const std::vector<uint32_t> prices{
      0x00000000u, 0x00000001u, 0x000000ffu, 0x00000100u,
      0x0000ffffu, 0x00010000u, 0x00ffffffu, 0x01000000u,
      0x7fffffffu, 0xfffffffeu, 0xffffffffu};

  for (uint32_t price : prices) {
    ASSERT_TRUE(index.ensure(price, Side::Bid).ok());
    ASSERT_TRUE(index.ensure(price, Side::Ask).ok());
  }

  std::vector<uint32_t> asks;
  for (auto point = index.best(Side::Ask); point;
       point = index.nextWorse(Side::Ask, point.price)) {
    asks.push_back(point.price);
  }
  EXPECT_EQ(asks, prices);

  std::vector<uint32_t> expected_bids(prices.rbegin(), prices.rend());
  std::vector<uint32_t> bids;
  for (auto point = index.best(Side::Bid); point;
       point = index.nextWorse(Side::Bid, point.price)) {
    bids.push_back(point.price);
  }
  EXPECT_EQ(bids, expected_bids);
}

TEST(PriceLevelIndexTest, NextWorseIsStrictEvenWhenStartingPriceIsAbsent) {
  auto arena = makeArena();
  PriceLevelIndex index(arena);
  ASSERT_TRUE(index.ensure(10, Side::Bid).ok());
  ASSERT_TRUE(index.ensure(20, Side::Bid).ok());
  ASSERT_TRUE(index.ensure(10, Side::Ask).ok());
  ASSERT_TRUE(index.ensure(20, Side::Ask).ok());

  const auto lower = index.nextWorse(Side::Bid, 15);
  const auto higher = index.nextWorse(Side::Ask, 15);
  ASSERT_TRUE(lower);
  ASSERT_TRUE(higher);
  EXPECT_EQ(lower.price, 10u);
  EXPECT_EQ(higher.price, 20u);
  EXPECT_FALSE(index.nextWorse(Side::Bid, 10));
  EXPECT_FALSE(index.nextWorse(Side::Ask, 20));
}

TEST(PriceLevelIndexTest, EnsureExistingDoesNotConsumeAnotherPoolSlot) {
  auto arena = makeArena(2, 1, 1);
  PriceLevelIndex index(arena);
  const auto created = index.ensure(123, Side::Bid);
  ASSERT_EQ(created.status, EnsureStatus::Created);

  const auto found = index.ensure(123, Side::Bid);
  EXPECT_EQ(found.status, EnsureStatus::Found);
  EXPECT_EQ(found.handle, created.handle);
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 2u);
  EXPECT_EQ(stats.leaves_in_use, 1u);
  EXPECT_EQ(stats.levels_in_use, 1u);
}

TEST(PriceLevelIndexTest, NonEmptyLevelCannotBeReclaimed) {
  auto arena = makeArena(2, 1, 1);
  PriceLevelIndex index(arena);
  const auto result = index.ensure(42, Side::Bid);
  ASSERT_TRUE(result.ok());
  PooledPriceLevel *level = arena.level(result.handle);
  ASSERT_NE(level, nullptr);
  level->num_orders = 1;
  level->head_idx = 7;
  level->tail_idx = 7;

  EXPECT_EQ(index.eraseEmpty(42, Side::Bid, result.handle),
            EraseStatus::LevelNotEmpty);
  EXPECT_EQ(index.find(42, Side::Bid), result.handle);
  EXPECT_EQ(arena.stats().levels_in_use, 1u);
}

TEST(PriceLevelIndexTest, ReclaimsCompletePathAndReusesFixedPools) {
  auto arena = makeArena(2, 1, 1);
  PriceLevelIndex index(arena);
  const auto first = index.ensure(0x00000001u, Side::Bid);
  ASSERT_EQ(first.status, EnsureStatus::Created);
  ASSERT_EQ(index.eraseEmpty(0x00000001u, Side::Bid, first.handle),
            EraseStatus::Erased);
  EXPECT_TRUE(index.empty(Side::Bid));
  EXPECT_EQ(arena.stats().internal_nodes_in_use, 0u);
  EXPECT_EQ(arena.stats().leaves_in_use, 0u);
  EXPECT_EQ(arena.stats().levels_in_use, 0u);

  // A completely different prefix can use the exact same fixed slots.
  const auto second = index.ensure(0xffffffffu, Side::Ask);
  ASSERT_EQ(second.status, EnsureStatus::Created);
  EXPECT_EQ(second.handle, first.handle);
  EXPECT_EQ(index.find(0xffffffffu, Side::Ask), second.handle);
  EXPECT_EQ(arena.stats().internal_node_high_watermark, 2u);
  EXPECT_EQ(arena.stats().leaf_high_watermark, 1u);
  EXPECT_EQ(arena.stats().level_high_watermark, 1u);
}

TEST(PriceLevelIndexTest, RemovingOneSidePreservesSharedOtherSidePath) {
  auto arena = makeArena(2, 1, 2);
  PriceLevelIndex index(arena);
  const auto bid = index.ensure(0x12345678u, Side::Bid);
  const auto ask = index.ensure(0x12345678u, Side::Ask);
  ASSERT_TRUE(bid.ok());
  ASSERT_TRUE(ask.ok());

  EXPECT_EQ(index.eraseEmpty(0x12345678u, Side::Bid, bid.handle),
            EraseStatus::Erased);
  EXPECT_FALSE(index.find(0x12345678u, Side::Bid));
  EXPECT_EQ(index.find(0x12345678u, Side::Ask), ask.handle);
  EXPECT_EQ(arena.stats().internal_nodes_in_use, 2u);
  EXPECT_EQ(arena.stats().leaves_in_use, 1u);
  EXPECT_EQ(arena.stats().levels_in_use, 1u);
}

TEST(PriceLevelIndexTest, InternalNodeExhaustionIsAtomicAndCounted) {
  auto arena = makeArena(1, 1, 1);
  PriceLevelIndex index(arena);

  const auto result = index.ensure(7, Side::Bid);
  EXPECT_EQ(result.status, EnsureStatus::InternalNodePoolExhausted);
  EXPECT_FALSE(result.handle);
  EXPECT_TRUE(index.empty(Side::Bid));
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 0u);
  EXPECT_EQ(stats.leaves_in_use, 0u);
  EXPECT_EQ(stats.levels_in_use, 0u);
  EXPECT_EQ(stats.internal_node_exhaustions, 1u);
}

TEST(PriceLevelIndexTest, LeafExhaustionIsAtomicAndCounted) {
  auto arena = makeArena(2, 0, 1);
  PriceLevelIndex index(arena);

  const auto result = index.ensure(7, Side::Bid);
  EXPECT_EQ(result.status, EnsureStatus::LeafPoolExhausted);
  EXPECT_TRUE(index.empty(Side::Bid));
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 0u);
  EXPECT_EQ(stats.leaves_in_use, 0u);
  EXPECT_EQ(stats.levels_in_use, 0u);
  EXPECT_EQ(stats.leaf_exhaustions, 1u);
}

TEST(PriceLevelIndexTest, LevelExhaustionIsAtomicAndCounted) {
  auto arena = makeArena(2, 1, 0);
  PriceLevelIndex index(arena);

  const auto result = index.ensure(7, Side::Bid);
  EXPECT_EQ(result.status, EnsureStatus::LevelPoolExhausted);
  EXPECT_TRUE(index.empty(Side::Bid));
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 0u);
  EXPECT_EQ(stats.leaves_in_use, 0u);
  EXPECT_EQ(stats.levels_in_use, 0u);
  EXPECT_EQ(stats.level_exhaustions, 1u);
}

TEST(PriceLevelIndexTest, FailedSecondSideCreationDoesNotDisturbFirstSide) {
  auto arena = makeArena(2, 1, 1);
  PriceLevelIndex index(arena);
  const auto bid = index.ensure(900, Side::Bid);
  ASSERT_TRUE(bid.ok());

  const auto ask = index.ensure(900, Side::Ask);
  EXPECT_EQ(ask.status, EnsureStatus::LevelPoolExhausted);
  EXPECT_EQ(index.find(900, Side::Bid), bid.handle);
  EXPECT_FALSE(index.find(900, Side::Ask));
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 2u);
  EXPECT_EQ(stats.leaves_in_use, 1u);
  EXPECT_EQ(stats.levels_in_use, 1u);
}

TEST(PriceLevelIndexTest, RollbackOnlyRemovesNewStillEmptyLevel) {
  auto arena = makeArena(2, 1, 1);
  PriceLevelIndex index(arena);
  const auto created = index.ensure(1234, Side::Bid);
  ASSERT_EQ(created.status, EnsureStatus::Created);
  EXPECT_TRUE(index.rollbackEnsure(1234, Side::Bid, created));
  EXPECT_TRUE(index.empty(Side::Bid));
  EXPECT_EQ(arena.stats().levels_in_use, 0u);

  const auto recreated = index.ensure(1234, Side::Bid);
  ASSERT_TRUE(recreated.ok());
  const auto found = index.ensure(1234, Side::Bid);
  ASSERT_EQ(found.status, EnsureStatus::Found);
  EXPECT_FALSE(index.rollbackEnsure(1234, Side::Bid, found));
  EXPECT_EQ(index.find(1234, Side::Bid), recreated.handle);
}

TEST(PriceLevelIndexTest, DestructorReturnsEverySharedPoolSlot) {
  auto arena = makeArena(16, 8, 8);
  {
    PriceLevelIndex index(arena);
    ASSERT_TRUE(index.ensure(1, Side::Bid).ok());
    ASSERT_TRUE(index.ensure(0xffffffffu, Side::Ask).ok());
    EXPECT_EQ(arena.stats().levels_in_use, 2u);
  }
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 0u);
  EXPECT_EQ(stats.leaves_in_use, 0u);
  EXPECT_EQ(stats.levels_in_use, 0u);
}

TEST(PriceLevelIndexTest, DifferentialOrderingAndReclamationAcrossSparseDomain) {
  auto arena = makeArena(2048, 1024, 1024);
  PriceLevelIndex index(arena);
  std::set<uint32_t> bid_prices;
  std::set<uint32_t> ask_prices;
  uint32_t state = 0x91e10da5u;
  for (size_t i = 0; i < 400; ++i) {
    state = state * 1664525u + 1013904223u;
    bid_prices.insert(state);
    ASSERT_TRUE(index.ensure(state, Side::Bid).ok());
    state = state * 1664525u + 1013904223u;
    ask_prices.insert(state);
    ASSERT_TRUE(index.ensure(state, Side::Ask).ok());
  }

  auto bid_it = bid_prices.rbegin();
  for (auto point = index.best(Side::Bid); point;
       point = index.nextWorse(Side::Bid, point.price), ++bid_it) {
    ASSERT_NE(bid_it, bid_prices.rend());
    EXPECT_EQ(point.price, *bid_it);
  }
  EXPECT_EQ(bid_it, bid_prices.rend());

  auto ask_it = ask_prices.begin();
  for (auto point = index.best(Side::Ask); point;
       point = index.nextWorse(Side::Ask, point.price), ++ask_it) {
    ASSERT_NE(ask_it, ask_prices.end());
    EXPECT_EQ(point.price, *ask_it);
  }
  EXPECT_EQ(ask_it, ask_prices.end());

  for (uint32_t price : bid_prices) {
    ASSERT_EQ(index.eraseEmpty(price, Side::Bid), EraseStatus::Erased);
  }
  for (uint32_t price : ask_prices) {
    ASSERT_EQ(index.eraseEmpty(price, Side::Ask), EraseStatus::Erased);
  }
  EXPECT_TRUE(index.empty(Side::Bid));
  EXPECT_TRUE(index.empty(Side::Ask));
  const auto stats = arena.stats();
  EXPECT_EQ(stats.internal_nodes_in_use, 0u);
  EXPECT_EQ(stats.leaves_in_use, 0u);
  EXPECT_EQ(stats.levels_in_use, 0u);
}
