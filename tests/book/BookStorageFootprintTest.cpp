#include "astra/book/BookManager.hpp"
#include "NasdaqItch20190130Fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {

constexpr std::size_t kTestPageBytes = 4u * 1024u;
constexpr const char *kEvidenceSha256 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::size_t descriptorSlotBytes() {
  return BookManager::kMaxStockLocate * sizeof(std::optional<OrderBook>);
}

} // namespace

TEST(BookStorageFootprintTest, CalculatesCompactLayoutWithoutMappingIt) {
  const BookStorageFootprint footprint = estimateBookStorageFootprint(
      BookManagerConfig::compact(), kTestPageBytes);

  EXPECT_EQ(footprint.system_page_bytes, 4'096u);
  EXPECT_EQ(footprint.order_direct_mapped_bytes, 16'777'216u);
  EXPECT_EQ(footprint.order_fallback_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_roots_mapped_bytes, 17'179'869'184ull);
  EXPECT_EQ(footprint.price_prepared_books_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_pages_mapped_bytes, 1'073'741'824ull);
  EXPECT_EQ(footprint.price_page_owners_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_page_summaries_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_page_occupancy_mapped_bytes, 10'485'760u);
  EXPECT_EQ(footprint.price_book_summaries_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_book_occupancy_mapped_bytes, 1'098'907'648ull);
  EXPECT_EQ(footprint.mapped_array_bytes, 19'390'267'392ull);
  EXPECT_EQ(footprint.descriptor_slot_bytes, descriptorSlotBytes());
  EXPECT_EQ(footprint.descriptor_mapped_bytes, 6'291'456u);
  EXPECT_EQ(footprint.planned_storage_bytes,
            footprint.mapped_array_bytes +
                footprint.descriptor_mapped_bytes);
}

TEST(BookStorageFootprintTest,
     CalculatesPinned20190130AcceptanceLayoutWithoutMappingIt) {
  const BookStorageFootprint footprint = estimateBookStorageFootprint(
      astra::benchmark_fixture::nasdaqItch20190130AcceptanceConfig(),
      kTestPageBytes);

  EXPECT_EQ(footprint.system_page_bytes, 4'096u);
  EXPECT_EQ(footprint.order_direct_mapped_bytes, 8'589'934'592ull);
  EXPECT_EQ(footprint.order_fallback_mapped_bytes, 67'108'864u);
  EXPECT_EQ(footprint.price_roots_mapped_bytes, 17'179'869'184ull);
  EXPECT_EQ(footprint.price_prepared_books_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_pages_mapped_bytes, 167'772'160'000ull);
  EXPECT_EQ(footprint.price_page_owners_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_page_summaries_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_page_occupancy_mapped_bytes, 1'342'177'280ull);
  EXPECT_EQ(footprint.price_book_summaries_mapped_bytes, 2'097'152u);
  EXPECT_EQ(footprint.price_book_occupancy_mapped_bytes, 1'098'907'648ull);
  EXPECT_EQ(footprint.mapped_array_bytes, 196'058'546'176ull);
  EXPECT_EQ(footprint.descriptor_slot_bytes, descriptorSlotBytes());
  EXPECT_EQ(footprint.descriptor_mapped_bytes, 6'291'456u);
  EXPECT_EQ(footprint.planned_storage_bytes,
            footprint.mapped_array_bytes +
                footprint.descriptor_mapped_bytes);
}

TEST(BookStorageFootprintTest,
     PinnedAcceptanceProfileCarriesTraceEvidenceAndExactHeadroom) {
  const BookCapacityProfile profile =
      astra::benchmark_fixture::nasdaqItch20190130AcceptanceProfile();

  EXPECT_EQ(profile.name, astra::benchmark_fixture::kNasdaqItch20190130Name);
  EXPECT_EQ(profile.evidence_sha256,
            astra::benchmark_fixture::kNasdaqItch20190130TraceSha256);
  EXPECT_EQ(profile.config.orders.direct_slots, std::uint64_t{1} << 29);
  EXPECT_EQ(profile.config.orders.fallback_bucket_count,
            std::uint32_t{1} << 20);
  EXPECT_EQ(profile.config.prices.page_capacity, 80'000u);
  EXPECT_EQ(profile.evidence.profiled_max_order_reference, 329'176'641u);
  EXPECT_EQ(profile.evidence.profiled_unique_price_pages, 68'941u);
  EXPECT_EQ(profile.minimum_headroom.direct_order_reference_slots,
            207'694'270u);
  EXPECT_EQ(profile.minimum_headroom.price_pages, 11'059u);

  const BookCapacityValidation validation =
      validateBookCapacityProfile(profile);
  EXPECT_EQ(validation.effective_headroom.direct_order_reference_slots,
            profile.minimum_headroom.direct_order_reference_slots);
  EXPECT_EQ(validation.effective_headroom.price_pages,
            profile.minimum_headroom.price_pages);
}

TEST(BookStorageFootprintTest,
     DeploymentProfileValidatesAbsoluteReserveWithoutMapping) {
  const BookCapacityProfile profile{
      "multiday-production-v1",
      kEvidenceSha256,
      {{1'024, 8, true}, {20, true}},
      {900, 10},
      {100, 5},
  };

  const BookCapacityValidation validation =
      validateBookCapacityProfile(profile);
  EXPECT_EQ(validation.effective_headroom.direct_order_reference_slots,
            123u);
  EXPECT_EQ(validation.effective_headroom.price_pages, 10u);
}

TEST(BookStorageFootprintTest,
     RejectsProfileWhoseObservedReferencesWouldUseFallback) {
  BookCapacityProfile profile{
      "too-small-direct", kEvidenceSha256,
      {{1'024, 8, false}, {20, false}},
      {1'024, 10}, {1, 1}};

  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.evidence.profiled_max_order_reference = 1'100;
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
}

TEST(BookStorageFootprintTest, RejectsInsufficientDirectProfileHeadroom) {
  const BookCapacityProfile profile{
      "too-little-direct-reserve", kEvidenceSha256,
      {{1'024, 8, false}, {20, false}}, {900, 10}, {124, 1}};

  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
}

TEST(BookStorageFootprintTest, RejectsInsufficientMonotonicPageHeadroom) {
  BookCapacityProfile profile{
      "too-many-pages", kEvidenceSha256,
      {{1'024, 8, false}, {20, false}},
      {900, 20}, {1, 1}};

  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.evidence.profiled_unique_price_pages = 15;
  profile.minimum_headroom.price_pages = 6;
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
}

TEST(BookStorageFootprintTest, RejectsProfileWithoutEvidenceOrReserve) {
  BookCapacityProfile profile{
      "missing-evidence", kEvidenceSha256,
      {{1'024, 8, false}, {20, false}},
      {0, 10}, {1, 1}};

  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.evidence = {900, 0};
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.evidence = {900, 10};
  profile.minimum_headroom = {0, 1};
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.minimum_headroom = {1, 0};
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.minimum_headroom = {1, 1};
  profile.name = {};
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.name = "missing-evidence-sha";
  profile.evidence_sha256 = {};
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
  profile.evidence_sha256 = "ABCDEF";
  EXPECT_THROW(validateBookCapacityProfile(profile), std::invalid_argument);
}

TEST(BookStorageFootprintTest, EveryMappedHotArenaUsesWholeHugePageExtents) {
  constexpr std::size_t huge_page_bytes = 2u * 1024u * 1024u;
  const BookStorageFootprint footprint = estimateBookStorageFootprint(
      astra::benchmark_fixture::nasdaqItch20190130AcceptanceConfig(),
      kTestPageBytes);
  const std::array arenas{
      footprint.order_direct_mapped_bytes,
      footprint.order_fallback_mapped_bytes,
      footprint.price_roots_mapped_bytes,
      footprint.price_prepared_books_mapped_bytes,
      footprint.price_pages_mapped_bytes,
      footprint.price_page_owners_mapped_bytes,
      footprint.price_page_summaries_mapped_bytes,
      footprint.price_page_occupancy_mapped_bytes,
      footprint.price_book_summaries_mapped_bytes,
      footprint.price_book_occupancy_mapped_bytes,
      footprint.descriptor_mapped_bytes,
  };

  for (const std::size_t mapped_bytes : arenas) {
    EXPECT_NE(mapped_bytes, 0u);
    EXPECT_EQ(mapped_bytes % huge_page_bytes, 0u);
  }
}

TEST(BookStorageFootprintTest, RejectsElementByteOverflowBeforeMapping) {
  BookManagerConfig config = BookManagerConfig::compact();
  config.orders.direct_slots =
      std::numeric_limits<std::size_t>::max() /
          sizeof(astra::book::OrderState) +
      1;

  EXPECT_THROW(estimateBookStorageFootprint(config, kTestPageBytes),
               std::length_error);
}

TEST(BookStorageFootprintTest, RejectsPageRoundingOverflowBeforeMapping) {
  BookManagerConfig config = BookManagerConfig::compact();
  config.orders.direct_slots =
      std::numeric_limits<std::size_t>::max() /
      sizeof(astra::book::OrderState);

  EXPECT_THROW(estimateBookStorageFootprint(config, kTestPageBytes),
               std::length_error);
}

TEST(BookStorageFootprintTest, AppliesConstructorConfigValidation) {
  BookManagerConfig config = BookManagerConfig::compact();
  config.prices.page_capacity =
      std::numeric_limits<astra::book::PricePageHandle>::max();

  EXPECT_THROW(estimateBookStorageFootprint(config, kTestPageBytes),
               std::invalid_argument);

  config = BookManagerConfig::compact();
  EXPECT_THROW(estimateBookStorageFootprint(config, 4'095u),
               std::invalid_argument);
}
