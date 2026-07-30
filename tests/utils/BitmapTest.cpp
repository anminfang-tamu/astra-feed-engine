#include "astra/utils/PriceBitmap.hpp"

#include <gtest/gtest.h>

TEST(PriceBitmapTest, SetAndFindLowestAtOrAbove) {
  PriceBitmap bitmap;

  bitmap.set(5);
  bitmap.set(10);
  bitmap.set(70);

  EXPECT_EQ(bitmap.findLowestAtOrAbove(0), 5u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(6), 10u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(11), 70u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(71), PriceBitmap::kInvalid);
}

TEST(PriceBitmapTest, SetAndFindHighestAtOrBelow) {
  PriceBitmap bitmap;

  bitmap.set(5);
  bitmap.set(10);
  bitmap.set(70);

  EXPECT_EQ(bitmap.findHighestAtOrBelow(71), 70u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(11), 10u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(6), 5u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(4), PriceBitmap::kInvalid);
}

TEST(PriceBitmapTest, Reset) {
  PriceBitmap bitmap;

  bitmap.set(5);
  bitmap.set(10);
  bitmap.set(70);

  bitmap.reset(10);

  EXPECT_EQ(bitmap.findLowestAtOrAbove(0), 5u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(6), 70u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(71), 70u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(9), 5u);
}

TEST(PriceBitmapTest, SearchesAcrossEveryHierarchyBoundary) {
  PriceBitmap bitmap;
  for (const uint32_t price :
       {0u, 63u, 64u, 4095u, 4096u, 65534u, 65535u}) {
    bitmap.set(price);
  }

  EXPECT_EQ(bitmap.findLowestAtOrAbove(1), 63u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(64), 64u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(65), 4095u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(4096), 4096u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(4097), 65534u);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(65535), 65535u);

  EXPECT_EQ(bitmap.findHighestAtOrBelow(65534), 65534u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(65533), 4096u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(4095), 4095u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(4094), 64u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(63), 63u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(62), 0u);
}

TEST(PriceBitmapTest, DuplicateSetAndResetAreIdempotent) {
  PriceBitmap bitmap;

  bitmap.set(4096);
  bitmap.set(4096);
  EXPECT_TRUE(bitmap.test(4096));
  EXPECT_EQ(bitmap.findLowestAtOrAbove(0), 4096u);

  bitmap.reset(4096);
  bitmap.reset(4096);
  EXPECT_FALSE(bitmap.test(4096));
  EXPECT_TRUE(bitmap.empty());
}

TEST(PriceBitmapTest, EmptyAndEndpointSearchesAreExplicit) {
  PriceBitmap bitmap;

  EXPECT_EQ(bitmap.findLowestAtOrAbove(0), PriceBitmap::kInvalid);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(PriceBitmap::kNumBits),
            PriceBitmap::kInvalid);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(0), PriceBitmap::kInvalid);

  bitmap.set(0);
  bitmap.set(PriceBitmap::kNumBits - 1);
  EXPECT_EQ(bitmap.findLowestAtOrAbove(0), 0u);
  EXPECT_EQ(bitmap.findHighestAtOrBelow(PriceBitmap::kNumBits - 1),
            PriceBitmap::kNumBits - 1);
}

TEST(PriceBitmapTest, SearchWorkHasACompileTimeFixedBound) {
  struct CountingObserver {
    uint32_t word_loads{0};
    void onWordLoad() noexcept { ++word_loads; }
  };

  // Force the longest route: query one end while the only bit is in a
  // different L2 subtree at the opposite end.
  static_assert(PriceBitmap::kMaxSearchWordLoads == 5);

  PriceBitmap low;
  low.set(0);
  CountingObserver predecessor;
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelowObserved(
                low.storage(), PriceBitmap::kNumBits - 1, predecessor),
            0u);
  EXPECT_EQ(predecessor.word_loads,
            PriceBitmap::kMaxSearchWordLoads);

  PriceBitmap high;
  high.set(PriceBitmap::kNumBits - 1);
  CountingObserver successor;
  EXPECT_EQ(PriceBitmap::findLowestAtOrAboveObserved(
                high.storage(), 0, successor),
            PriceBitmap::kNumBits - 1);
  EXPECT_EQ(successor.word_loads, PriceBitmap::kMaxSearchWordLoads);

  CountingObserver same_word;
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelowObserved(
                high.storage(), PriceBitmap::kNumBits - 1, same_word),
            PriceBitmap::kNumBits - 1);
  EXPECT_EQ(same_word.word_loads, 1u);
}

TEST(PriceBitmapTest, TrivialStorageCanLiveInAZeroedArena) {
  PriceBitmapStorage storage{};
  EXPECT_TRUE(PriceBitmap::empty(storage));

  PriceBitmap::set(storage, 65535);
  EXPECT_TRUE(PriceBitmap::test(storage, 65535));
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelow(storage, 65535), 65535u);

  PriceBitmap::reset(storage, 65535);
  EXPECT_TRUE(PriceBitmap::empty(storage));
}

TEST(PriceBitmapTest, InvalidL2BitsAreMaskedBeforeChildIndexing) {
  PriceBitmapStorage storage{};
  storage.l2 = uint64_t{1} << 63;

  EXPECT_TRUE(PriceBitmap::empty(storage));
  EXPECT_EQ(PriceBitmap::findLowestAtOrAbove(storage, 0),
            PriceBitmap::kInvalid);
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelow(
                storage, PriceBitmap::kNumBits - 1),
            PriceBitmap::kInvalid);
}

TEST(PriceBitmapTest, StaleSummaryBitsWithZeroChildrenFailClosed) {
  PriceBitmapStorage stale_l2{};
  stale_l2.l2 = uint64_t{1} << 3;
  EXPECT_EQ(PriceBitmap::findLowestAtOrAbove(stale_l2, 0),
            PriceBitmap::kInvalid);
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelow(
                stale_l2, PriceBitmap::kNumBits - 1),
            PriceBitmap::kInvalid);

  PriceBitmapStorage stale_l1{};
  stale_l1.l2 = uint64_t{1} << 3;
  stale_l1.l1[3] = uint64_t{1} << 7;
  EXPECT_EQ(PriceBitmap::findLowestAtOrAbove(stale_l1, 0),
            PriceBitmap::kInvalid);
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelow(
                stale_l1, PriceBitmap::kNumBits - 1),
            PriceBitmap::kInvalid);

  PriceBitmapStorage same_l1{};
  same_l1.l1[0] = uint64_t{1} << 1;
  EXPECT_EQ(PriceBitmap::findLowestAtOrAbove(same_l1, 0),
            PriceBitmap::kInvalid);
  EXPECT_EQ(PriceBitmap::findHighestAtOrBelow(same_l1, 127),
            PriceBitmap::kInvalid);
}
