#include "astra/utils/FixedMmapArray.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

namespace {

struct Record {
  std::uint64_t key;
  std::uint32_t value;
  std::uint32_t flags;
};

static_assert(std::is_trivial_v<Record>);
static_assert(std::is_trivially_copyable_v<Record>);

TEST(FixedMmapArrayTest, CreatesZeroedArrayObjectsAndIndexesThem) {
  FixedMmapArray<Record> records(257);

  ASSERT_EQ(records.size(), 257u);
  ASSERT_NE(records.data(), nullptr);
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(records[index].key, 0u);
    EXPECT_EQ(records[index].value, 0u);
    EXPECT_EQ(records[index].flags, 0u);
  }

  records[0] = Record{11u, 22u, 33u};
  records[256] = Record{44u, 55u, 66u};
  EXPECT_EQ(records[0].value, 22u);
  EXPECT_EQ(records[256].key, 44u);
}

TEST(FixedMmapArrayTest, MoveTransfersOneArrayLifetime) {
  FixedMmapArray<Record> source(4);
  source[3] = Record{7u, 8u, 9u};
  Record *const original = source.data();

  FixedMmapArray<Record> destination(std::move(source));
  EXPECT_EQ(source.data(), nullptr);
  EXPECT_EQ(source.size(), 0u);
  EXPECT_EQ(destination.data(), original);
  EXPECT_EQ(destination[3].flags, 9u);
}

TEST(FixedMmapArrayTest, ByteOverflowFailsBeforeMapping) {
  const std::size_t overflowing =
      std::numeric_limits<std::size_t>::max() / sizeof(Record) + 1;
  EXPECT_DEATH((void)FixedMmapArray<Record>(overflowing), "");
}

} // namespace
