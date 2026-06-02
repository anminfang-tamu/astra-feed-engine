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
