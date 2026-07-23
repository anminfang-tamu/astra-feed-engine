#pragma once

#include "astra/book/MutationResult.hpp"
#include "astra/protocol/OrderSide.hpp"
#include "astra/utils/MappedArray.hpp"
#include "astra/utils/PriceBitmap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace astra::book {

using PricePageHandle = std::uint32_t;

inline constexpr PricePageHandle kInvalidPricePageHandle = 0;
inline constexpr std::uint32_t kPricePartBits = 16;
inline constexpr std::uint32_t kPricePartCount = 1u << kPricePartBits;
inline constexpr std::uint64_t kPriceRootSlotCount =
    std::uint64_t{1} << (kPricePartBits * 2);
inline constexpr std::size_t kPricePageBytes = 2u * 1024u * 1024u;
inline constexpr std::uint8_t kTopLevelCount = 10;

struct PriceParts {
  std::uint16_t page_index;
  std::uint16_t level_index;
};

constexpr PriceParts splitPrice(std::uint32_t price) noexcept {
  return PriceParts{static_cast<std::uint16_t>(price >> kPricePartBits),
                    static_cast<std::uint16_t>(price & 0xffffu)};
}

constexpr std::uint32_t joinPrice(std::uint16_t page_index,
                                  std::uint16_t level_index) noexcept {
  return (static_cast<std::uint32_t>(page_index) << kPricePartBits) |
         static_cast<std::uint32_t>(level_index);
}

constexpr std::uint64_t priceRootIndex(std::uint16_t stock_locate,
                                       std::uint16_t page_index) noexcept {
  return (static_cast<std::uint64_t>(stock_locate) << kPricePartBits) |
         static_cast<std::uint64_t>(page_index);
}

struct alignas(16) PriceLevelState {
  std::uint64_t total_qty;
  std::uint32_t order_count;
  std::uint32_t reserved;
};

static_assert(sizeof(PriceLevelState) == 16);
static_assert(alignof(PriceLevelState) == 16);
static_assert(std::is_trivial_v<PriceLevelState>);
static_assert(64 % sizeof(PriceLevelState) == 0);

// Bids and asks are deliberately separated.  A mutation knows its side, so a
// fetched cache line contains four useful adjacent levels from that side.
struct alignas(64) PricePage {
  std::array<PriceLevelState, kPricePartCount> bids;
  std::array<PriceLevelState, kPricePartCount> asks;
};

static_assert(sizeof(PricePage) == kPricePageBytes);
static_assert(alignof(PricePage) == 64);
static_assert(offsetof(PricePage, asks) == 1024u * 1024u);
static_assert(std::is_trivial_v<PricePage>);

struct PriceAddress {
  PricePageHandle page_handle{kInvalidPricePageHandle};
  std::uint16_t level_index{0};
  std::uint16_t reserved{0};

  constexpr bool valid() const noexcept {
    return page_handle != kInvalidPricePageHandle;
  }
};

static_assert(sizeof(PriceAddress) == 8);

// Reserving a new page is read-only.  commitReservation() is the sole method
// that advances the arena and publishes a root handle.  The expected page
// count detects stale/out-of-order reservations in the single-writer engine.
struct PricePageReservation {
  MutationResult result{MutationResult::InvariantViolation};
  PriceAddress address{};
  std::uint16_t stock_locate{0};
  std::uint16_t page_index{0};
  std::uint32_t expected_committed_pages{0};
  bool requires_commit{false};

  constexpr bool valid() const noexcept {
    return result == MutationResult::Applied && address.valid();
  }
};

struct PriceLevelCursor {
  PricePageHandle page_handle{kInvalidPricePageHandle};
  std::uint32_t raw_price{0};
  // Expected owner of page_handle. It occupies padding already present in the
  // cursor and lets read traversal reject a cross-book cached-handle alias.
  std::uint16_t stock_locate{0};
  bool valid{false};
};

static_assert(sizeof(PriceLevelCursor) == 12);

struct PriceLevelView {
  std::uint32_t raw_price{0};
  std::uint64_t total_qty{0};
  std::uint32_t order_count{0};
  bool valid{false};
};

struct PriceLevelStoreConfig {
  std::uint32_t page_capacity{0};
  bool prefault{false};
};

struct PriceLevelStoreCounters {
  std::uint32_t page_capacity{0};
  std::uint32_t committed_pages{0};
  std::uint32_t prepared_books{0};
  std::uint64_t page_capacity_failures{0};
};

// Narrow friend seam used by focused corruption tests. Production code has no
// root-mutation API; roots are published only by commitReservation().
struct PriceLevelStoreTestPeer;

class PriceLevelStore final {
public:
  // Structural work contracts used by portable performance tests.  They are
  // compile-time properties, not counters added to the production hot path.
  static constexpr std::uint32_t kReservePriceRootSlotReads = 1;
  static constexpr std::uint32_t kCommitReservationRootSlotReads = 1;
  static constexpr std::uint32_t kCommitReservationRootSlotWrites = 1;
  static constexpr std::uint32_t kNewPriceTransactionRootSlotReads =
      kReservePriceRootSlotReads + kCommitReservationRootSlotReads;
  static constexpr std::uint32_t kExistingOrderRootLookups = 0;
  static constexpr std::uint32_t kCachedBestBitmapSearches = 0;
  static constexpr std::uint32_t kMaxNextWorseBitmapSearches = 2;
  static constexpr std::uint32_t kSingletonPageErasePageBitmapOperations = 0;
  static constexpr std::uint32_t kMaxTopTenNextWorseCalls =
      kTopLevelCount - 1;
  static constexpr std::uint32_t kMaxTopTenBitmapSearches =
      kMaxTopTenNextWorseCalls * kMaxNextWorseBitmapSearches;
  // Public storage-layout constants let startup planning account for the
  // private hot-summary and cold-bitmap arenas without exposing their
  // implementation records.
  static constexpr std::size_t kSideCount = 2;
  static constexpr std::size_t kHotArenaAlignment = kPricePageBytes;
  static constexpr std::size_t kPageOwnerStorageBytes = 8;
  static constexpr std::size_t kPageOwnerStorageAlignment =
      alignof(std::uint32_t);
  static constexpr std::size_t kPageSummaryStorageBytes = 16;
  static constexpr std::size_t kPageSummaryStorageAlignment = 16;
  static constexpr std::size_t kBookSummaryStorageBytes = 32;
  static constexpr std::size_t kBookSummaryStorageAlignment = 32;
  static constexpr std::size_t kBitmapStorageBytes =
      sizeof(PriceBitmapStorage);
  static constexpr std::size_t kBitmapStorageAlignment =
      alignof(PriceBitmapStorage);

  explicit PriceLevelStore(PriceLevelStoreConfig config);

  PriceLevelStore(const PriceLevelStore &) = delete;
  PriceLevelStore &operator=(const PriceLevelStore &) = delete;
  PriceLevelStore(PriceLevelStore &&) = delete;
  PriceLevelStore &operator=(PriceLevelStore &&) = delete;

  MutationResult prepareBook(std::uint16_t stock_locate) noexcept;
  bool isBookPrepared(std::uint16_t stock_locate) const noexcept;

  PricePageReservation
  reservePrice(std::uint16_t stock_locate,
               std::uint32_t raw_price) const noexcept;
  MutationResult
  commitReservation(const PricePageReservation &reservation) noexcept;

  MutationResult add(PriceAddress address, OrderSide side,
                     std::uint32_t qty) noexcept;
  MutationResult reduce(PriceAddress address, OrderSide side,
                        std::uint32_t current_order_qty,
                        std::uint32_t reduction_qty) noexcept;
  // Existing-order hot path: validate both dimensions of the resident page
  // owner and perform the partial aggregate reduction as one checked store
  // operation. The expected page index comes from the 16-byte OrderState.
  MutationResult reduceChecked(std::uint16_t expected_stock_locate,
                               std::uint16_t expected_page_index,
                               PriceAddress address, OrderSide side,
                               std::uint32_t current_order_qty,
                               std::uint32_t reduction_qty) noexcept;
  MutationResult erase(PriceAddress address, OrderSide side,
                       std::uint32_t remaining_order_qty) noexcept;
  MutationResult move(PriceAddress old_address, std::uint32_t old_qty,
                      PriceAddress new_address, std::uint32_t new_qty,
                      OrderSide side) noexcept;

  PriceLevelState *resolveLevel(PriceAddress address,
                                OrderSide side) noexcept;
  const PriceLevelState *resolveLevel(PriceAddress address,
                                      OrderSide side) const noexcept;

  PriceLevelCursor best(std::uint16_t stock_locate,
                        OrderSide side) const noexcept;
  PriceLevelCursor nextWorse(std::uint16_t stock_locate, OrderSide side,
                             PriceLevelCursor current) const noexcept;
  PriceLevelView view(PriceLevelCursor cursor,
                      OrderSide side) const noexcept;
  std::uint8_t topTen(std::uint16_t stock_locate, OrderSide side,
                      std::span<PriceLevelView> output) const noexcept;

  PricePageHandle pageHandle(std::uint16_t stock_locate,
                             std::uint16_t page_index) const noexcept;
  bool ownsAddress(std::uint16_t stock_locate,
                   std::uint16_t page_index,
                   PriceAddress address) const noexcept;
  const PricePage *page(PricePageHandle handle) const noexcept;
  // Startup/acceptance diagnostics use the exact VMA range to prove actual
  // huge-page residency. Feed processing never calls these accessors.
  const void *rootsArenaBase() const noexcept { return roots_.data(); }
  std::size_t rootsArenaMappedBytes() const noexcept {
    return roots_.mappedBytes();
  }
  const void *preparedBooksArenaBase() const noexcept {
    return prepared_books_.data();
  }
  std::size_t preparedBooksArenaMappedBytes() const noexcept {
    return prepared_books_.mappedBytes();
  }
  const void *pageArenaBase() const noexcept { return pages_.data(); }
  std::size_t pageArenaMappedBytes() const noexcept {
    return pages_.mappedBytes();
  }
  const void *pageOwnersArenaBase() const noexcept {
    return page_owners_.data();
  }
  std::size_t pageOwnersArenaMappedBytes() const noexcept {
    return page_owners_.mappedBytes();
  }
  const void *pageSummariesArenaBase() const noexcept {
    return page_summaries_.data();
  }
  std::size_t pageSummariesArenaMappedBytes() const noexcept {
    return page_summaries_.mappedBytes();
  }
  const void *pageOccupancyArenaBase() const noexcept {
    return page_occupancy_.data();
  }
  std::size_t pageOccupancyArenaMappedBytes() const noexcept {
    return page_occupancy_.mappedBytes();
  }
  const void *bookSummariesArenaBase() const noexcept {
    return book_summaries_.data();
  }
  std::size_t bookSummariesArenaMappedBytes() const noexcept {
    return book_summaries_.mappedBytes();
  }
  const void *bookOccupancyArenaBase() const noexcept {
    return book_occupancy_.data();
  }
  std::size_t bookOccupancyArenaMappedBytes() const noexcept {
    return book_occupancy_.mappedBytes();
  }

  PriceLevelStoreCounters counters() const noexcept;

private:
  friend struct PriceLevelStoreTestPeer;

  struct PageOwner {
    std::uint16_t stock_locate;
    std::uint16_t page_index;
    std::uint32_t committed;
  };

  struct PageSideSummary {
    std::uint32_t best_level_index;
    std::uint32_t active_levels;
  };

  // Four page summaries tile one cache line. Page owners remain in their own
  // compact 8-byte arena so the dominant partial E/X path does not expand its
  // owner-validation working set.
  struct alignas(16) PageSummary {
    std::array<PageSideSummary, kSideCount> sides;
  };

  struct BookSideSummary {
    std::uint32_t best_raw_price;
    PricePageHandle best_page_handle;
    std::uint32_t active_pages;
  };

  // Two book summaries tile one cache line. Page-index occupancy remains in a
  // separate fixed arena.
  struct alignas(32) BookSummary {
    std::array<BookSideSummary, kSideCount> sides;
    std::array<std::byte, 8> padding;
  };

  struct DeactivationPlan {
    std::uint32_t next_level_index{PriceBitmap::kInvalid};
    std::uint32_t next_book_raw_price{0};
    PricePageHandle next_book_page_handle{kInvalidPricePageHandle};
    bool page_becomes_empty{false};
    bool page_best_changes{false};
    bool book_best_changes{false};
  };

  static_assert(sizeof(PageSideSummary) == 8);
  static_assert(sizeof(BookSideSummary) == 12);
  static_assert(sizeof(PageOwner) == kPageOwnerStorageBytes);
  static_assert(alignof(PageOwner) == kPageOwnerStorageAlignment);
  static_assert(sizeof(PageSummary) == kPageSummaryStorageBytes);
  static_assert(sizeof(BookSummary) == kBookSummaryStorageBytes);
  static_assert(alignof(PageSummary) == kPageSummaryStorageAlignment);
  static_assert(alignof(BookSummary) == kBookSummaryStorageAlignment);
  static_assert(std::is_trivial_v<PageOwner>);
  static_assert(std::is_trivial_v<PageSideSummary>);
  static_assert(std::is_trivial_v<BookSideSummary>);
  static_assert(std::is_trivial_v<PageSummary>);
  static_assert(std::is_trivial_v<BookSummary>);

  static bool validSide(OrderSide side) noexcept;
  static std::uint32_t sideIndex(OrderSide side) noexcept;
  static bool better(OrderSide side, std::uint32_t lhs,
                     std::uint32_t rhs) noexcept;

  std::uint64_t pageSideIndex(PricePageHandle handle,
                              OrderSide side) const noexcept;
  std::uint32_t bookSideIndex(std::uint16_t stock_locate,
                              OrderSide side) const noexcept;
  PageSideSummary *pageSummary(PricePageHandle handle,
                               OrderSide side) noexcept;
  const PageSideSummary *pageSummary(PricePageHandle handle,
                                     OrderSide side) const noexcept;
  PriceBitmapStorage *pageOccupancy(PricePageHandle handle,
                                    OrderSide side) noexcept;
  const PriceBitmapStorage *pageOccupancy(PricePageHandle handle,
                                          OrderSide side) const noexcept;
  BookSideSummary *bookSummary(std::uint16_t stock_locate,
                               OrderSide side) noexcept;
  const BookSideSummary *bookSummary(std::uint16_t stock_locate,
                                     OrderSide side) const noexcept;
  PriceBitmapStorage *bookOccupancy(std::uint16_t stock_locate,
                                    OrderSide side) noexcept;
  const PriceBitmapStorage *bookOccupancy(std::uint16_t stock_locate,
                                          OrderSide side) const noexcept;
  PriceLevelState *levelUnchecked(PriceAddress address,
                                  OrderSide side) noexcept;
  const PriceLevelState *levelUnchecked(PriceAddress address,
                                        OrderSide side) const noexcept;
  PageOwner *owner(PricePageHandle handle) noexcept;
  const PageOwner *owner(PricePageHandle handle) const noexcept;
  bool rootHandleMatches(std::uint16_t stock_locate,
                         std::uint16_t page_index,
                         PricePageHandle handle) const noexcept;

  MutationResult validateAddress(PriceAddress address,
                                 OrderSide side) const noexcept;
  MutationResult validateActivation(PriceAddress address,
                                    OrderSide side) const noexcept;
  MutationResult validateAdd(PriceAddress address, OrderSide side,
                             std::uint32_t qty) const noexcept;
  MutationResult validateErase(PriceAddress address, OrderSide side,
                               std::uint32_t qty,
                               DeactivationPlan &plan) const noexcept;
  MutationResult validateDeactivation(PriceAddress address, OrderSide side,
                                      DeactivationPlan &plan) const noexcept;
  void activateLevel(PriceAddress address, OrderSide side) noexcept;
  void deactivateLevel(PriceAddress address, OrderSide side,
                       const DeactivationPlan &plan) noexcept;
  void addValidated(PriceAddress address, OrderSide side,
                    std::uint32_t qty) noexcept;
  void eraseValidated(PriceAddress address, OrderSide side,
                      std::uint32_t qty,
                      const DeactivationPlan &plan) noexcept;

  PriceLevelStoreConfig config_;
  astra::utils::MappedArray<PricePageHandle> roots_;
  astra::utils::MappedArray<std::uint8_t> prepared_books_;
  astra::utils::MappedArray<PricePage> pages_;
  astra::utils::MappedArray<PageOwner> page_owners_;
  astra::utils::MappedArray<PageSummary> page_summaries_;
  astra::utils::MappedArray<PriceBitmapStorage> page_occupancy_;
  astra::utils::MappedArray<BookSummary> book_summaries_;
  astra::utils::MappedArray<PriceBitmapStorage> book_occupancy_;
  std::uint32_t committed_pages_{0};
  std::uint32_t prepared_book_count_{0};
  mutable std::uint64_t page_capacity_failures_{0};
};

} // namespace astra::book
