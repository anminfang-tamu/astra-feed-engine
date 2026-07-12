#include "astra/book/OrderArena.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

TEST(OrderArenaTest, RejectsCapacityOutsideTheIndexDomain) {
  EXPECT_THROW((void)OrderArena(0), std::invalid_argument);

  if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
    const size_t too_large =
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1;
    EXPECT_THROW((void)OrderArena(too_large), std::length_error);
  }
}

TEST(OrderArenaTest, ExhaustionIsExplicitAndCounted) {
  OrderArena arena(3);

  EXPECT_EQ(arena.allocate(), 0u);
  EXPECT_EQ(arena.allocate(), 1u);
  EXPECT_EQ(arena.allocate(), 2u);
  EXPECT_EQ(arena.allocate(), OrderArena::kInvalidIndex);
  EXPECT_EQ(arena.allocate(), OrderArena::kInvalidIndex);

  EXPECT_EQ(arena.capacity(), 3u);
  EXPECT_EQ(arena.inUse(), 3u);
  EXPECT_EQ(arena.highWatermark(), 3u);
  EXPECT_EQ(arena.allocationFailures(), 2u);
  EXPECT_EQ(arena.stats().allocation_failures, 2u);
}

TEST(OrderArenaTest, ReusesReleasedIndicesDeterministically) {
  OrderArena arena(4);
  const uint32_t zero = arena.allocate();
  const uint32_t one = arena.allocate();
  const uint32_t two = arena.allocate();

  ASSERT_TRUE(arena.release(one));
  EXPECT_EQ(arena.allocate(), one);
  ASSERT_TRUE(arena.release(two));
  ASSERT_TRUE(arena.release(zero));
  EXPECT_EQ(arena.allocate(), zero);
  EXPECT_EQ(arena.allocate(), two);
}

TEST(OrderArenaTest, AddressesRemainStableAcrossOtherOperations) {
  OrderArena arena(64);
  const uint32_t retained = arena.allocate();
  Order *const retained_address = arena.at(retained);
  ASSERT_NE(retained_address, nullptr);
  retained_address->order_id = 987654321;
  retained_address->price = 1234500;

  std::vector<uint32_t> temporary;
  for (size_t i = 0; i < 50; ++i) {
    temporary.push_back(arena.allocate());
  }
  for (uint32_t index : temporary) {
    ASSERT_TRUE(arena.release(index));
  }

  EXPECT_EQ(arena.at(retained), retained_address);
  EXPECT_EQ(arena.at(retained)->order_id, 987654321u);
  EXPECT_EQ(arena.at(retained)->price, 1234500u);
}

TEST(OrderArenaTest, DistinguishesInvalidAndDoubleReleases) {
  OrderArena arena(2);
  const uint32_t live = arena.allocate();

  EXPECT_FALSE(arena.release(OrderArena::kInvalidIndex));
  EXPECT_FALSE(arena.release(2));
  EXPECT_TRUE(arena.release(live));
  EXPECT_FALSE(arena.release(live));

  EXPECT_EQ(arena.invalidReleases(), 2u);
  EXPECT_EQ(arena.doubleReleases(), 1u);
  EXPECT_EQ(arena.inUse(), 0u);
  EXPECT_EQ(arena.at(live), nullptr);
}

TEST(OrderArenaTest, ReleasedRecordIsFullyResetBeforeReuse) {
  OrderArena arena(1);
  const uint32_t index = arena.allocate();
  Order *const order = arena.at(index);
  ASSERT_NE(order, nullptr);
  order->order_id = std::numeric_limits<uint64_t>::max();
  order->price = std::numeric_limits<uint64_t>::max() - 1;
  order->prev_idx = 11;
  order->next_idx = 12;
  order->qty = 13;
  order->level_and_side = std::numeric_limits<uint32_t>::max();

  ASSERT_TRUE(arena.release(index));
  ASSERT_EQ(arena.allocate(), index);
  const Order *const reused = arena.at(index);
  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(reused->order_id, 0u);
  EXPECT_EQ(reused->price, 0u);
  EXPECT_EQ(reused->prev_idx, 0u);
  EXPECT_EQ(reused->next_idx, 0u);
  EXPECT_EQ(reused->qty, 0u);
  EXPECT_EQ(reused->level_and_side, 0u);
}

TEST(OrderArenaTest, RandomizedAllocationMatchesReferenceFreeStack) {
  constexpr size_t kCapacity = 257;
  constexpr size_t kOperations = 100'000;
  OrderArena arena(kCapacity);
  std::vector<bool> live(kCapacity, false);
  std::vector<uint32_t> expected_free;
  expected_free.reserve(kCapacity);
  for (size_t i = 0; i < kCapacity; ++i) {
    expected_free.push_back(static_cast<uint32_t>(kCapacity - i - 1));
  }

  size_t expected_in_use = 0;
  size_t expected_high_watermark = 0;
  uint64_t expected_allocation_failures = 0;
  uint64_t expected_invalid_releases = 0;
  uint64_t expected_double_releases = 0;
  std::mt19937_64 random(0x4153545241ULL);

  for (size_t operation = 0; operation < kOperations; ++operation) {
    if ((random() & 1U) == 0) {
      const uint32_t actual = arena.allocate();
      if (expected_free.empty()) {
        EXPECT_EQ(actual, OrderArena::kInvalidIndex);
        ++expected_allocation_failures;
      } else {
        const uint32_t expected = expected_free.back();
        expected_free.pop_back();
        ASSERT_EQ(actual, expected);
        ASSERT_FALSE(live[actual]);
        live[actual] = true;
        ++expected_in_use;
        expected_high_watermark =
            std::max(expected_high_watermark, expected_in_use);
        ASSERT_NE(arena.at(actual), nullptr);
        arena.at(actual)->order_id = operation + 1;
      }
    } else {
      // Include both valid and deliberately out-of-range indices.
      const uint32_t index =
          static_cast<uint32_t>(random() % (kCapacity + 8));
      const bool actual = arena.release(index);
      if (index >= kCapacity) {
        EXPECT_FALSE(actual);
        ++expected_invalid_releases;
      } else if (!live[index]) {
        EXPECT_FALSE(actual);
        ++expected_double_releases;
      } else {
        EXPECT_TRUE(actual);
        live[index] = false;
        expected_free.push_back(index);
        --expected_in_use;
      }
    }

    ASSERT_EQ(arena.inUse(), expected_in_use);
    ASSERT_EQ(arena.highWatermark(), expected_high_watermark);
    ASSERT_EQ(arena.allocationFailures(), expected_allocation_failures);
    ASSERT_EQ(arena.invalidReleases(), expected_invalid_releases);
    ASSERT_EQ(arena.doubleReleases(), expected_double_releases);
    if ((operation & 0xffU) == 0) {
      for (size_t index = 0; index < kCapacity; ++index) {
        ASSERT_EQ(arena.isInUse(static_cast<uint32_t>(index)), live[index]);
        ASSERT_EQ(arena.at(static_cast<uint32_t>(index)) != nullptr,
                  live[index]);
      }
    }
  }
}
