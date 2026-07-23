#include "astra/book/OrderTable.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using astra::book::LookupPath;
using astra::book::MutationResult;
using astra::book::OrderSlotReservation;
using astra::book::OrderState;
using astra::book::OrderTable;
using astra::book::OrderTableConfig;

OrderState state(std::uint32_t qty, std::uint16_t locate = 7) {
  return OrderState{qty, 11, 22, locate,
                    static_cast<std::uint8_t>(OrderSide::Buy), 0, 0};
}

void insert(OrderTable &table, std::uint64_t ref, OrderState value) {
  const OrderSlotReservation reservation = table.reserve(ref);
  ASSERT_EQ(reservation.result, MutationResult::Applied);
  ASSERT_EQ(table.commit(reservation, value), MutationResult::Applied);
}

std::vector<std::uint64_t> colliders(OrderTable &table,
                                     std::uint32_t primary,
                                     std::size_t count) {
  std::vector<std::uint64_t> result;
  for (std::uint64_t ref = table.directCapacity(); result.size() < count;
       ++ref) {
    if (table.fallbackCandidateBuckets(ref)[0] == primary)
      result.push_back(ref);
  }
  return result;
}

} // namespace

TEST(OrderTableTest, OrderStateTilesCacheLinesWithoutStraddling) {
  static_assert(sizeof(OrderState) == 16);
  static_assert(alignof(OrderState) == 16);
  static_assert(64 % sizeof(OrderState) == 0);
  static_assert(std::is_trivially_copyable_v<OrderState>);

  OrderTable table(OrderTableConfig{8, 2, false});
  const auto address = reinterpret_cast<std::uintptr_t>(table.directData());
  EXPECT_EQ(address % 64, 0u);
  EXPECT_EQ(&table.directData()[1] - &table.directData()[0], 1);
  EXPECT_EQ(reinterpret_cast<const std::byte *>(&table.directData()[1]) -
                reinterpret_cast<const std::byte *>(&table.directData()[0]),
            16);
}

TEST(OrderTableTest, DirectReferencesUseTheirExactArraySlot) {
  static_assert(OrderTable::kDirectSlotsExamined == 1);
  OrderTable table(OrderTableConfig{8, 2, false});
  insert(table, 0, state(10));
  insert(table, 7, state(70));

  const auto zero = table.find(0);
  const auto last = table.find(7);
  ASSERT_TRUE(zero.found());
  ASSERT_TRUE(last.found());
  EXPECT_EQ(zero.path, LookupPath::Direct);
  EXPECT_EQ(last.path, LookupPath::Direct);
  EXPECT_EQ(zero.state, &table.directData()[0]);
  EXPECT_EQ(last.state, &table.directData()[7]);
  EXPECT_EQ(last.state->qty, 70u);

  const auto first_fallback = table.reserve(8);
  EXPECT_NE(first_fallback.path, LookupPath::Direct);
}

TEST(OrderTableTest, HotLookupReturnsExactDirectAddressOrNull) {
  OrderTable table(OrderTableConfig{8, 2, false});
  insert(table, 3, state(30));

  const auto diagnostic = table.find(3);
  ASSERT_TRUE(diagnostic.found());
  EXPECT_EQ(table.findState(3), diagnostic.state);
  EXPECT_EQ(table.findState(3), &table.directData()[3]);
  EXPECT_EQ(table.findState(4), nullptr);

  const OrderTable &const_table = table;
  EXPECT_EQ(const_table.findState(3), diagnostic.state);
  EXPECT_EQ(const_table.findState(4), nullptr);
}

TEST(OrderTableTest, ArbitraryUint64ReferenceUsesBoundedFallback) {
  OrderTable table(OrderTableConfig{8, 4, false});
  constexpr std::uint64_t ref = std::numeric_limits<std::uint64_t>::max();
  insert(table, ref, state(99, 65'535));

  const auto found = table.find(ref);
  ASSERT_TRUE(found.found());
  EXPECT_TRUE(found.path == LookupPath::FallbackPrimary ||
              found.path == LookupPath::FallbackSecondary);
  EXPECT_EQ(found.state->qty, 99u);
  EXPECT_EQ(found.state->locate, 65'535u);
}

TEST(OrderTableTest, DuplicateReservationPreservesOriginalState) {
  OrderTable table(OrderTableConfig{8, 2, false});
  insert(table, 3, state(100));

  const auto duplicate = table.reserve(3);
  EXPECT_EQ(duplicate.result, MutationResult::DuplicateOrderRef);
  EXPECT_EQ(table.commit(duplicate, state(999)),
            MutationResult::DuplicateOrderRef);
  ASSERT_TRUE(table.find(3).found());
  EXPECT_EQ(table.find(3).state->qty, 100u);
}

TEST(OrderTableTest, EraseAndReuseDoNotShiftAnyOtherRecord) {
  OrderTable table(OrderTableConfig{8, 2, false});
  const auto refs = colliders(table, 0, 3);
  insert(table, refs[0], state(10));
  insert(table, refs[1], state(20));
  insert(table, refs[2], state(30));
  const OrderState *const second_before = table.find(refs[1]).state;

  ASSERT_EQ(table.erase(refs[0]), MutationResult::Applied);
  EXPECT_EQ(table.find(refs[0]).result, MutationResult::OrderNotFound);
  EXPECT_EQ(table.find(refs[1]).state, second_before);

  const auto replacement = table.reserve(refs[0]);
  ASSERT_EQ(replacement.result, MutationResult::Applied);
  ASSERT_EQ(table.commit(replacement, state(40)), MutationResult::Applied);
  EXPECT_EQ(table.find(refs[0]).state->qty, 40u);
}

TEST(OrderTableTest, FallbackUsesAtMostTwoFixedTwoWayBuckets) {
  struct CountingObserver {
    std::uint32_t slots_examined{0};
    void onFallbackSlot() noexcept { ++slots_examined; }
  };

  static_assert(OrderTable::kFallbackCandidateBuckets == 2);
  static_assert(OrderTable::kFallbackWays == 2);
  static_assert(OrderTable::kMaxFallbackSlotsExamined == 4);

  OrderTable table(OrderTableConfig{8, 2, false});
  const auto refs = colliders(table, 0, 5);

  insert(table, refs[0], state(10));
  insert(table, refs[1], state(20));
  insert(table, refs[2], state(30));
  insert(table, refs[3], state(40));

  EXPECT_EQ(table.find(refs[0]).path, LookupPath::FallbackPrimary);
  EXPECT_EQ(table.find(refs[2]).path, LookupPath::FallbackSecondary);

  const auto full = table.reserve(refs[4]);
  EXPECT_EQ(full.result, MutationResult::OrderTableCapacityExceeded);
  EXPECT_FALSE(table.find(refs[4]).found());
  CountingObserver observer;
  EXPECT_EQ(table.findFallbackStateObserved(refs[4], observer), nullptr);
  EXPECT_EQ(observer.slots_examined,
            OrderTable::kMaxFallbackSlotsExamined);
  EXPECT_EQ(table.find(refs[0]).state->qty, 10u);
  EXPECT_EQ(table.find(refs[3]).state->qty, 40u);
}

TEST(OrderTableTest, HotLookupMatchesFallbackDiagnosticAndMiss) {
  OrderTable table(OrderTableConfig{8, 2, false});
  const auto refs = colliders(table, 0, 5);
  insert(table, refs[0], state(10));
  insert(table, refs[1], state(20));
  insert(table, refs[2], state(30));
  insert(table, refs[3], state(40));

  const auto primary = table.find(refs[0]);
  const auto secondary = table.find(refs[2]);
  ASSERT_EQ(primary.path, LookupPath::FallbackPrimary);
  ASSERT_EQ(secondary.path, LookupPath::FallbackSecondary);
  EXPECT_EQ(table.findState(refs[0]), primary.state);
  EXPECT_EQ(table.findState(refs[2]), secondary.state);
  EXPECT_EQ(table.findState(refs[4]), nullptr);

  const OrderTable &const_table = table;
  EXPECT_EQ(const_table.findState(refs[0]), primary.state);
  EXPECT_EQ(const_table.findState(refs[2]), secondary.state);
  EXPECT_EQ(const_table.findState(refs[4]), nullptr);
}

TEST(OrderTableTest, StaleReservationCannotOverwriteCommittedRecord) {
  OrderTable table(OrderTableConfig{8, 2, false});
  const auto reservation = table.reserve(5);
  ASSERT_TRUE(reservation.valid());
  insert(table, 5, state(50));

  EXPECT_EQ(table.commit(reservation, state(500)),
            MutationResult::StaleReservation);
  EXPECT_EQ(table.find(5).state->qty, 50u);
}

TEST(OrderTableTest, FallbackCommitRevalidatesCandidateBucketAndPath) {
  OrderTable table(OrderTableConfig{8, 8, false});
  constexpr std::uint64_t ref = 8;
  const auto reservation = table.reserve(ref);
  ASSERT_TRUE(reservation.valid());
  ASSERT_TRUE(reservation.path == LookupPath::FallbackPrimary ||
              reservation.path == LookupPath::FallbackSecondary);

  const auto candidates = table.fallbackCandidateBuckets(ref);
  std::uint32_t non_candidate = 0;
  while (non_candidate == candidates[0] ||
         non_candidate == candidates[1]) {
    ++non_candidate;
  }

  auto forged_bucket = reservation;
  forged_bucket.bucket = non_candidate;
  EXPECT_EQ(table.commit(forged_bucket, state(80)),
            MutationResult::StaleReservation);
  EXPECT_FALSE(table.find(ref).found());

  auto forged_path = reservation;
  forged_path.path = reservation.path == LookupPath::FallbackPrimary
                         ? LookupPath::FallbackSecondary
                         : LookupPath::FallbackPrimary;
  EXPECT_EQ(table.commit(forged_path, state(80)),
            MutationResult::StaleReservation);
  EXPECT_FALSE(table.find(ref).found());

  EXPECT_EQ(table.commit(reservation, state(80)), MutationResult::Applied);
  EXPECT_EQ(table.find(ref).state->qty, 80u);
}

TEST(OrderTableTest, SingleFallbackBucketRejectsSecondaryReservationPath) {
  OrderTable table(OrderTableConfig{8, 1, false});
  constexpr std::uint64_t ref = 8;
  const auto reservation = table.reserve(ref);
  ASSERT_TRUE(reservation.valid());
  ASSERT_EQ(reservation.path, LookupPath::FallbackPrimary);

  auto forged = reservation;
  forged.path = LookupPath::FallbackSecondary;
  EXPECT_EQ(table.commit(forged, state(80)),
            MutationResult::StaleReservation);
  EXPECT_FALSE(table.find(ref).found());
}

TEST(OrderTableTest, InvalidConfigurationFailsBeforeFeedProcessing) {
  EXPECT_THROW((OrderTable{OrderTableConfig{0, 2, false}}),
               std::invalid_argument);
  EXPECT_THROW((OrderTable{OrderTableConfig{8, 3, false}}),
               std::invalid_argument);
}
