#include "astra/book/LocalOrderRefMap.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace {

using InsertResult = LocalOrderRefMap::InsertResult;
using ReplaceResult = LocalOrderRefMap::ReplaceResult;

} // namespace

TEST(LocalOrderRefMapTest, StoresLocalIndicesAcrossFullUint64Range) {
  LocalOrderRefMap map(16);
  constexpr uint64_t high_bit_ref = uint64_t{1} << 63;
  constexpr uint64_t max_ref = std::numeric_limits<uint64_t>::max();

  EXPECT_EQ(map.insert(1, 0), InsertResult::Inserted);
  EXPECT_EQ(map.insert(high_bit_ref, 17), InsertResult::Inserted);
  EXPECT_EQ(map.insert(max_ref, std::numeric_limits<uint32_t>::max() - 1),
            InsertResult::Inserted);

  EXPECT_EQ(map.find(1), 0u);
  EXPECT_EQ(map.find(high_bit_ref), 17u);
  EXPECT_EQ(map.find(max_ref), std::numeric_limits<uint32_t>::max() - 1);
  EXPECT_EQ(map.size(), 3u);
  EXPECT_EQ(map.capacity(), 16u);
  EXPECT_EQ(map.maxEntries(), 8u);
}

TEST(LocalOrderRefMapTest, DuplicateDoesNotOverwriteExistingIndex) {
  LocalOrderRefMap map(8);

  ASSERT_EQ(map.insert(42, 11), InsertResult::Inserted);
  EXPECT_EQ(map.insert(42, 22), InsertResult::Duplicate);

  EXPECT_EQ(map.find(42), 11u);
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map.failureCounters().duplicate_inserts, 1u);
}

TEST(LocalOrderRefMapTest, BackwardShiftDeletePreservesWrappedProbeChain) {
  LocalOrderRefMap map(8);

  // With SplitMix64 and eight slots, these references share home slot seven.
  ASSERT_EQ(map.insert(7, 11), InsertResult::Inserted);
  ASSERT_EQ(map.insert(13, 22), InsertResult::Inserted);
  ASSERT_EQ(map.insert(16, 33), InsertResult::Inserted);
  ASSERT_GE(map.probeStats().max_length, 3u);

  EXPECT_TRUE(map.erase(7));
  EXPECT_EQ(map.find(7), LocalOrderRefMap::kInvalidIndex);
  EXPECT_EQ(map.find(13), 22u);
  EXPECT_EQ(map.find(16), 33u);

  EXPECT_TRUE(map.erase(13));
  EXPECT_EQ(map.find(16), 33u);
  EXPECT_EQ(map.size(), 1u);
}

TEST(LocalOrderRefMapTest, ReplaceKeyPreservesIndexAtMaximumLoad) {
  LocalOrderRefMap map(4);
  constexpr uint64_t replacement = std::numeric_limits<uint64_t>::max();

  ASSERT_EQ(map.insert(10, 11), InsertResult::Inserted);
  ASSERT_EQ(map.insert(20, 22), InsertResult::Inserted);
  ASSERT_EQ(map.size(), map.maxEntries());

  EXPECT_EQ(map.replaceKey(10, replacement), ReplaceResult::Replaced);
  EXPECT_EQ(map.find(10), LocalOrderRefMap::kInvalidIndex);
  EXPECT_EQ(map.find(replacement), 11u);
  EXPECT_EQ(map.find(20), 22u);
  EXPECT_EQ(map.size(), 2u);

  EXPECT_EQ(map.replaceKey(replacement, 20), ReplaceResult::Duplicate);
  EXPECT_EQ(map.find(replacement), 11u);
  EXPECT_EQ(map.find(20), 22u);
  EXPECT_EQ(map.failureCounters().duplicate_replacements, 1u);
}

TEST(LocalOrderRefMapTest, EnforcesHalfLoadAndReportsFailures) {
  LocalOrderRefMap map(4);

  ASSERT_EQ(map.insert(1, 11), InsertResult::Inserted);
  ASSERT_EQ(map.insert(2, 22), InsertResult::Inserted);
  EXPECT_DOUBLE_EQ(map.loadFactor(), 0.5);

  EXPECT_EQ(map.insert(3, 33), InsertResult::Full);
  EXPECT_EQ(map.insert(1, 44), InsertResult::Duplicate);
  EXPECT_EQ(map.failureCounters().full_inserts, 1u);
  EXPECT_EQ(map.failureCounters().duplicate_inserts, 1u);

  EXPECT_FALSE(map.erase(99));
  EXPECT_EQ(map.replaceKey(99, 100), ReplaceResult::NotFound);
  EXPECT_EQ(map.failureCounters().erase_misses, 1u);
  EXPECT_EQ(map.failureCounters().missing_replacements, 1u);

  ASSERT_TRUE(map.erase(2));
  EXPECT_EQ(map.insert(3, 33), InsertResult::Inserted);
  EXPECT_EQ(map.find(3), 33u);
}

TEST(LocalOrderRefMapTest, RejectsReservedKeyAndIndex) {
  LocalOrderRefMap map(8);

  EXPECT_EQ(map.insert(0, 1), InsertResult::Invalid);
  EXPECT_EQ(map.insert(1, LocalOrderRefMap::kInvalidIndex),
            InsertResult::Invalid);
  EXPECT_EQ(map.find(0), LocalOrderRefMap::kInvalidIndex);
  EXPECT_FALSE(map.contains(0));
  EXPECT_FALSE(map.erase(0));
  EXPECT_EQ(map.replaceKey(1, 0), ReplaceResult::Invalid);

  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.failureCounters().invalid_inserts, 2u);
  EXPECT_EQ(map.failureCounters().invalid_erases, 1u);
  EXPECT_EQ(map.failureCounters().invalid_replacements, 1u);
}

TEST(LocalOrderRefMapTest, ReportsCurrentProbeLayoutWithoutHotPathCounters) {
  LocalOrderRefMap map(8);

  ASSERT_EQ(map.insert(7, 11), InsertResult::Inserted);  // one slot
  ASSERT_EQ(map.insert(13, 22), InsertResult::Inserted); // two slots
  EXPECT_EQ(map.find(16), LocalOrderRefMap::kInvalidIndex); // three-slot miss

  const LocalOrderRefMap::ProbeStats stats = map.probeStats();
  EXPECT_EQ(stats.searches, 2u);
  EXPECT_EQ(stats.slots_examined, 3u);
  EXPECT_EQ(stats.max_length, 2u);
  EXPECT_DOUBLE_EQ(stats.averageLength(), 1.5);
}

TEST(LocalOrderRefMapTest, RejectsNonPowerOfTwoSlotCapacity) {
  EXPECT_THROW((void)LocalOrderRefMap(0), std::invalid_argument);
  EXPECT_THROW((void)LocalOrderRefMap(3), std::invalid_argument);
}

TEST(LocalOrderRefMapTest, RandomizedOperationsMatchReferenceMap) {
  constexpr size_t kSlots = 256;
  constexpr size_t kOperations = 50'000;
  LocalOrderRefMap map(kSlots);
  std::unordered_map<uint64_t, uint32_t> reference;
  std::mt19937_64 random(0x4c4f43414cULL);

  const auto randomKey = [&random]() noexcept { return random() | uint64_t{1}; };
  const auto randomIndex = [&random]() noexcept {
    return static_cast<uint32_t>(random() & 0x7fff'ffffU);
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
      const uint32_t index = randomIndex();
      const bool duplicate = reference.contains(key);
      const bool full = reference.size() >= map.maxEntries();
      const InsertResult expected =
          duplicate ? InsertResult::Duplicate
                    : (full ? InsertResult::Full : InsertResult::Inserted);
      ASSERT_EQ(map.insert(key, index), expected);
      if (expected == InsertResult::Inserted) {
        reference.emplace(key, index);
      }
    } else if (choice == 1) {
      const bool erase_existing = (random() & 1U) != 0;
      const uint64_t key = erase_existing ? existingKey() : randomKey();
      const bool expected = reference.erase(key) != 0;
      ASSERT_EQ(map.erase(key), expected);
    } else {
      const uint64_t old_key = existingKey();
      const uint64_t new_key =
          (random() & 3U) == 0 ? existingKey() : randomKey();
      const bool duplicate = new_key != old_key && reference.contains(new_key);
      const ReplaceResult expected = duplicate ? ReplaceResult::Duplicate
                                               : ReplaceResult::Replaced;
      ASSERT_EQ(map.replaceKey(old_key, new_key), expected);
      if (expected == ReplaceResult::Replaced && old_key != new_key) {
        const uint32_t index = reference.at(old_key);
        reference.erase(old_key);
        reference.emplace(new_key, index);
      }
    }

    ASSERT_EQ(map.size(), reference.size());
    if ((operation & 0xffU) == 0) {
      for (const auto &[key, index] : reference) {
        ASSERT_EQ(map.find(key), index);
      }
    }
  }
}
