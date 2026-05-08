#include "astra/utils/Bitmap.hpp"

#include <gtest/gtest.h>

namespace {
const uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();
}

TEST(BitmapTest, SetAndFindNext) {
  Bitmap bitmap;

  bitmap.set(5);
  bitmap.set(10);
  bitmap.set(70);

  EXPECT_EQ(bitmap.findNextSet(0), 5u);
  EXPECT_EQ(bitmap.findNextSet(6), 10u);
  EXPECT_EQ(bitmap.findNextSet(11), 70u);
  EXPECT_EQ(bitmap.findNextSet(71), kNoIndex);
}

TEST(BitmapTest, SetAndFindPrev) {
  Bitmap bitmap;

  bitmap.set(5);
  bitmap.set(10);
  bitmap.set(70);

  EXPECT_EQ(bitmap.findPrevSet(71), 70u);
  EXPECT_EQ(bitmap.findPrevSet(11), 10u);
  EXPECT_EQ(bitmap.findPrevSet(6), 5u);
  EXPECT_EQ(bitmap.findPrevSet(4), kNoIndex);
}

TEST(BitmapTest, Reset) {
  Bitmap bitmap;

  bitmap.set(5);
  bitmap.set(10);
  bitmap.set(70);

  bitmap.reset(10);

  EXPECT_EQ(bitmap.findNextSet(0), 5u);
  EXPECT_EQ(bitmap.findNextSet(6), 70u);
  EXPECT_EQ(bitmap.findPrevSet(71), 70u);
  EXPECT_EQ(bitmap.findPrevSet(9), 5u);
}
