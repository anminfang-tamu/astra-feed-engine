#include "astra/utils/OrderIdMap.hpp"

#include <gtest/gtest.h>

TEST(OrderIdMapTest, ErasePreservesProbeChainAcrossNaturalSlot) {
  OrderIdMap index(8);

  ASSERT_TRUE(index.insert(1, 10));
  ASSERT_TRUE(index.insert(4, 20));
  ASSERT_TRUE(index.insert(11, 30));

  ASSERT_TRUE(index.erase(1));

  EXPECT_EQ(index.find(1), OrderIdMap::kInvalidIdx);
  EXPECT_EQ(index.find(4), 20u);
  EXPECT_EQ(index.find(11), 30u);
}

TEST(OrderIdMapTest, DuplicateInsertDoesNotUpdateExistingPoolIndex) {
  OrderIdMap index(8);

  ASSERT_TRUE(index.insert(42, 10));

  EXPECT_FALSE(index.insert(42, 20));
  EXPECT_EQ(index.find(42), 10u);
}

TEST(OrderIdMapTest, SupportsZeroOrderRefBecauseStateTracksOccupancy) {
  OrderIdMap index(8);

  ASSERT_TRUE(index.insert(0, 10));

  EXPECT_EQ(index.find(0), 10u);
  EXPECT_TRUE(index.erase(0));
  EXPECT_EQ(index.find(0), OrderIdMap::kInvalidIdx);
}

TEST(OrderIdMapTest, ClearRemovesEntriesWithoutReallocating) {
  OrderIdMap index(8);

  ASSERT_TRUE(index.insert(1, 10));
  ASSERT_TRUE(index.insert(11, 30));

  index.clear();

  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.capacity(), 8u);
  EXPECT_EQ(index.find(1), OrderIdMap::kInvalidIdx);
  EXPECT_EQ(index.find(11), OrderIdMap::kInvalidIdx);
}
