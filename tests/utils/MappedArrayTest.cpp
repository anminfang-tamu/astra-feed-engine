#include "astra/utils/MappedArray.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

std::size_t pageSize() {
  const long value = ::sysconf(_SC_PAGESIZE);
  if (value <= 0)
    throw std::runtime_error("unable to determine system page size");
  return static_cast<std::size_t>(value);
}

#if defined(__APPLE__)
using MincoreByte = char;
#else
using MincoreByte = unsigned char;
#endif

std::vector<MincoreByte> residency(const void *address, std::size_t bytes) {
  const std::size_t page_size = pageSize();
  const std::size_t pages = (bytes + page_size - 1) / page_size;
  std::vector<MincoreByte> result(pages);
  if (::mincore(const_cast<void *>(address), pages * page_size,
                result.data()) != 0) {
    throw std::runtime_error("mincore failed");
  }
  return result;
}

TEST(MappedArrayTest, IsZeroFilledAndHonorsTwoMiBAlignment) {
  constexpr std::size_t alignment = 2u * 1024u * 1024u;
  astra::utils::MappedArray<std::uint64_t> values(1'024, alignment);

  ASSERT_NE(values.data(), nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(values.data()) % alignment, 0u);
  EXPECT_EQ(values.alignment(), alignment);
  EXPECT_EQ(values.mappedBytes(), alignment);
  for (std::size_t index = 0; index < values.size(); ++index)
    ASSERT_EQ(values[index], 0u);
}

TEST(MappedArrayTest, TwoMiBAlignmentPadsBothEndsToWholeHugePages) {
  constexpr std::size_t alignment = 2u * 1024u * 1024u;
  astra::utils::MappedArray<std::uint8_t> values(alignment + 1, alignment);

  ASSERT_NE(values.data(), nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(values.data()) % alignment, 0u);
  EXPECT_EQ(values.bytes(), alignment + 1);
  EXPECT_EQ(values.mappedBytes(), 2 * alignment);
  EXPECT_EQ(values.mappedBytes() % alignment, 0u);
}

TEST(MappedArrayTest, WholeArenaPrefaultIncludesHugePagePadding) {
  constexpr std::size_t alignment = 2u * 1024u * 1024u;
  astra::utils::MappedArray<std::uint8_t> values(alignment + 1, alignment);

  values.prefault();

  const auto resident = residency(values.data(), values.mappedBytes());
  ASSERT_EQ(resident.size(), values.mappedBytes() / pageSize());
  for (MincoreByte state : resident)
    EXPECT_NE(static_cast<unsigned int>(state) & 1u, 0u);
}

TEST(MappedArrayTest, PrefaultMakesEveryLogicalPageResident) {
  const std::size_t page_size = pageSize();
  constexpr std::size_t page_count = 8;
  astra::utils::MappedArray<std::uint8_t> values(page_size * page_count);

  values.prefault();

  const auto resident = residency(values.data(), values.bytes());
  ASSERT_EQ(resident.size(), page_count);
  for (MincoreByte state : resident)
    EXPECT_NE(static_cast<unsigned int>(state) & 1u, 0u);
}

TEST(MappedArrayTest, RangePrefaultClampsAtLogicalEnd) {
  const std::size_t page_size = pageSize();
  astra::utils::MappedArray<std::uint8_t> values(page_size * 3);

  values.prefault(page_size * 2, page_size * 4);

  const auto resident = residency(values.data(), values.bytes());
  ASSERT_EQ(resident.size(), 3u);
  EXPECT_NE(static_cast<unsigned int>(resident[2]) & 1u, 0u);
}

TEST(MappedArrayTest, MoveTransfersTheMappingExactlyOnce) {
  astra::utils::MappedArray<std::uint32_t> source(128);
  source[17] = 42;
  std::uint32_t *const original = source.data();

  astra::utils::MappedArray<std::uint32_t> destination(std::move(source));

  EXPECT_EQ(source.data(), nullptr);
  EXPECT_EQ(source.size(), 0u);
  EXPECT_EQ(destination.data(), original);
  EXPECT_EQ(destination[17], 42u);
}

TEST(MappedArrayTest, RejectsAlignmentThatIsNotAPageMultiplePowerOfTwo) {
  EXPECT_THROW((astra::utils::MappedArray<std::uint32_t>(16, pageSize() * 3)),
               std::invalid_argument);
}

} // namespace
