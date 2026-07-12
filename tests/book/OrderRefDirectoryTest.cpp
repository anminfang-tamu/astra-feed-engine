#include "astra/book/OrderRefDirectory.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace {

using InsertResult = OrderRefDirectory::InsertResult;
using ReplaceResult = OrderRefDirectory::ReplaceResult;

constexpr OrderRefDirectory::Handle kHandleA{7, 11};
constexpr OrderRefDirectory::Handle kHandleB{9, 22};
constexpr OrderRefDirectory::Handle kHandleC{11, 33};

} // namespace

TEST(OrderRefDirectoryTest, StoresSparseReferencesAcrossFullUint64Range) {
  OrderRefDirectory directory(16);
  constexpr uint64_t high_bit_ref = uint64_t{1} << 63;
  constexpr uint64_t max_ref = std::numeric_limits<uint64_t>::max();

  EXPECT_EQ(directory.insert(1, kHandleA.stock_locate, kHandleA.pool_index),
            InsertResult::Inserted);
  EXPECT_EQ(directory.insert(high_bit_ref, kHandleB.stock_locate,
                             kHandleB.pool_index),
            InsertResult::Inserted);
  EXPECT_EQ(directory.insert(max_ref, kHandleC.stock_locate,
                             kHandleC.pool_index),
            InsertResult::Inserted);

  EXPECT_EQ(directory.find(1), kHandleA);
  EXPECT_EQ(directory.find(high_bit_ref), kHandleB);
  EXPECT_EQ(directory.find(max_ref), kHandleC);
  EXPECT_EQ(directory.size(), 3u);
  EXPECT_EQ(directory.capacity(), 16u);
  EXPECT_EQ(directory.maxEntries(), 8u);
}

TEST(OrderRefDirectoryTest, DuplicateDoesNotOverwriteExistingHandle) {
  OrderRefDirectory directory(8);

  ASSERT_EQ(directory.insert(42, kHandleA.stock_locate, kHandleA.pool_index),
            InsertResult::Inserted);
  EXPECT_EQ(directory.insert(42, kHandleB.stock_locate, kHandleB.pool_index),
            InsertResult::Duplicate);

  EXPECT_EQ(directory.find(42), kHandleA);
  EXPECT_EQ(directory.size(), 1u);
  EXPECT_EQ(directory.failureCounters().duplicate_inserts, 1u);
}

TEST(OrderRefDirectoryTest, BackwardShiftDeletePreservesWrappedProbeChain) {
  OrderRefDirectory directory(8);

  // With the directory hash and eight slots, 7, 13, and 16 all have home slot
  // seven. Their probe chain wraps through slots zero and one.
  ASSERT_EQ(directory.insert(7, kHandleA.stock_locate, kHandleA.pool_index),
            InsertResult::Inserted);
  ASSERT_EQ(directory.insert(13, kHandleB.stock_locate, kHandleB.pool_index),
            InsertResult::Inserted);
  ASSERT_EQ(directory.insert(16, kHandleC.stock_locate, kHandleC.pool_index),
            InsertResult::Inserted);
  ASSERT_GE(directory.maxProbeLength(), 3u);

  EXPECT_TRUE(directory.erase(7));
  EXPECT_FALSE(directory.find(7).valid());
  EXPECT_EQ(directory.find(13), kHandleB);
  EXPECT_EQ(directory.find(16), kHandleC);

  EXPECT_TRUE(directory.erase(13));
  EXPECT_EQ(directory.find(16), kHandleC);
  EXPECT_EQ(directory.size(), 1u);
}

TEST(OrderRefDirectoryTest, ReplaceKeyPreservesHandleAtMaximumLoad) {
  OrderRefDirectory directory(4);
  constexpr uint64_t replacement = std::numeric_limits<uint64_t>::max();

  ASSERT_EQ(directory.insert(10, kHandleA.stock_locate, kHandleA.pool_index),
            InsertResult::Inserted);
  ASSERT_EQ(directory.insert(20, kHandleB.stock_locate, kHandleB.pool_index),
            InsertResult::Inserted);
  ASSERT_EQ(directory.size(), directory.maxEntries());

  EXPECT_EQ(directory.replaceKey(10, replacement), ReplaceResult::Replaced);
  EXPECT_FALSE(directory.find(10).valid());
  EXPECT_EQ(directory.find(replacement), kHandleA);
  EXPECT_EQ(directory.find(20), kHandleB);
  EXPECT_EQ(directory.size(), 2u);

  EXPECT_EQ(directory.replaceKey(replacement, 20), ReplaceResult::Duplicate);
  EXPECT_EQ(directory.find(replacement), kHandleA);
  EXPECT_EQ(directory.find(20), kHandleB);
  EXPECT_EQ(directory.failureCounters().duplicate_replacements, 1u);
}

TEST(OrderRefDirectoryTest, EnforcesHalfLoadAndReportsCapacityFailure) {
  OrderRefDirectory directory(4);

  ASSERT_EQ(directory.insert(1, kHandleA.stock_locate, kHandleA.pool_index),
            InsertResult::Inserted);
  ASSERT_EQ(directory.insert(2, kHandleB.stock_locate, kHandleB.pool_index),
            InsertResult::Inserted);
  EXPECT_DOUBLE_EQ(directory.loadFactor(), 0.5);

  EXPECT_EQ(directory.insert(3, kHandleC.stock_locate, kHandleC.pool_index),
            InsertResult::Full);
  EXPECT_EQ(directory.insert(1, kHandleC.stock_locate, kHandleC.pool_index),
            InsertResult::Duplicate);
  EXPECT_EQ(directory.failureCounters().full_inserts, 1u);
  EXPECT_EQ(directory.failureCounters().duplicate_inserts, 1u);
  EXPECT_EQ(directory.size(), directory.maxEntries());

  ASSERT_TRUE(directory.erase(2));
  EXPECT_EQ(directory.insert(3, kHandleC.stock_locate, kHandleC.pool_index),
            InsertResult::Inserted);
  EXPECT_EQ(directory.find(3), kHandleC);
}

TEST(OrderRefDirectoryTest, RejectsZeroKeyAndInvalidHandles) {
  OrderRefDirectory directory(8);

  EXPECT_EQ(directory.insert(0, kHandleA.stock_locate, kHandleA.pool_index),
            InsertResult::Invalid);
  EXPECT_EQ(directory.insert(1, 0, kHandleA.pool_index),
            InsertResult::Invalid);
  EXPECT_EQ(directory.insert(2, kHandleA.stock_locate,
                             OrderRefDirectory::kInvalidIdx),
            InsertResult::Invalid);
  EXPECT_FALSE(directory.find(0).valid());
  EXPECT_FALSE(directory.erase(0));
  EXPECT_EQ(directory.replaceKey(1, 0), ReplaceResult::Invalid);
  EXPECT_TRUE(directory.empty());
  EXPECT_EQ(directory.failureCounters().invalid_inserts, 3u);
  EXPECT_EQ(directory.failureCounters().erase_misses, 1u);
  EXPECT_EQ(directory.failureCounters().invalid_replacements, 1u);
}

TEST(OrderRefDirectoryTest, RejectsNonPowerOfTwoSlotCapacity) {
  EXPECT_THROW((void)OrderRefDirectory(0), std::invalid_argument);
  EXPECT_THROW((void)OrderRefDirectory(3), std::invalid_argument);
}

TEST(OrderRefDirectoryTest, RandomizedOperationsMatchReferenceMap) {
  constexpr size_t kSlots = 256;
  constexpr size_t kOperations = 50'000;
  OrderRefDirectory directory(kSlots);
  std::unordered_map<uint64_t, OrderRefDirectory::Handle> reference;
  std::mt19937_64 random(0x4153545241ULL);

  const auto randomKey = [&random]() noexcept { return random() | uint64_t{1}; };
  const auto randomHandle = [&random]() noexcept {
    return OrderRefDirectory::Handle{
        static_cast<uint16_t>((random() % 16'383) + 1),
        static_cast<uint32_t>(random() & 0x7fff'ffffU)};
  };
  const auto existingKey = [&]() {
    auto it = reference.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(random() % reference.size()));
    return it->first;
  };

  for (size_t operation = 0; operation < kOperations; ++operation) {
    const uint64_t choice = random() % 3;
    if (choice == 0 || reference.empty()) {
      const uint64_t key = randomKey();
      const OrderRefDirectory::Handle handle = randomHandle();
      const bool duplicate = reference.contains(key);
      const bool full = reference.size() >= directory.maxEntries();
      const InsertResult expected =
          duplicate ? InsertResult::Duplicate
                    : (full ? InsertResult::Full : InsertResult::Inserted);
      ASSERT_EQ(directory.insert(key, handle.stock_locate, handle.pool_index),
                expected);
      if (expected == InsertResult::Inserted) {
        reference.emplace(key, handle);
      }
    } else if (choice == 1) {
      const bool erase_existing = (random() & 1U) != 0;
      const uint64_t key = erase_existing ? existingKey() : randomKey();
      const bool expected = reference.erase(key) != 0;
      ASSERT_EQ(directory.erase(key), expected);
    } else {
      const uint64_t old_key = existingKey();
      const uint64_t new_key = (random() & 3U) == 0 ? existingKey()
                                                    : randomKey();
      const bool duplicate = new_key != old_key && reference.contains(new_key);
      const ReplaceResult expected = duplicate ? ReplaceResult::Duplicate
                                               : ReplaceResult::Replaced;
      ASSERT_EQ(directory.replaceKey(old_key, new_key), expected);
      if (expected == ReplaceResult::Replaced && old_key != new_key) {
        const OrderRefDirectory::Handle handle = reference.at(old_key);
        reference.erase(old_key);
        reference.emplace(new_key, handle);
      }
    }

    ASSERT_EQ(directory.size(), reference.size());
    if ((operation & 0xffU) == 0) {
      for (const auto &[key, handle] : reference) {
        ASSERT_EQ(directory.find(key), handle);
      }
    }
  }
}
