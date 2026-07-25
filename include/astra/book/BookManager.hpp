#pragma once

#include "astra/book/MutationResult.hpp"
#include "astra/book/OrderBook.hpp"
#include "astra/book/OrderTable.hpp"
#include "astra/book/PriceLevelStore.hpp"
#include "astra/symbol/StockDirectory.hpp"
#include "astra/symbol/StockLocate.hpp"
#include "astra/utils/MappedObjectArray.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct BookManagerConfig {
  astra::book::OrderTableConfig orders{};
  astra::book::PriceLevelStoreConfig prices{512, false};

  static constexpr BookManagerConfig compact() noexcept { return {}; }
};

// Capacity evidence is measured over the entire lifetime of one process.
// Price pages are monotonic handles and are not reclaimed, so the page value
// is the number of distinct (stock_locate, page_index) pairs ever observed,
// not the peak number concurrently active.
struct BookCapacityEvidence {
  std::uint64_t profiled_max_order_reference{0};
  std::uint32_t profiled_unique_price_pages{0};
};

// Absolute reserve avoids floating-point and rounding differences during
// deterministic startup admission. The direct reserve is the number of
// direct-index slots above profiled_max_order_reference; anomalous larger
// references still use the separately sized, bounded fallback table.
struct BookCapacityHeadroom {
  std::uint64_t direct_order_reference_slots{0};
  std::uint32_t price_pages{0};
};

struct BookCapacityProfile {
  // Deployment profiles are startup-only objects, so own their audit
  // metadata rather than exposing string_view lifetime hazards to callers.
  std::string name;
  std::string evidence_sha256;
  BookManagerConfig config;
  BookCapacityEvidence evidence;
  BookCapacityHeadroom minimum_headroom;
};

// Canonical custom-profile evidence is an exact UTF-8/ASCII, LF-terminated
// key/value manifest. The loader hashes the bytes it parses, verifies that hash
// against the deployment's independently configured expected digest, and makes
// the manifest's capacity/evidence values authoritative.
struct BookCapacityEvidenceManifest {
  static constexpr std::string_view kSchemaV1 =
      "astra_book_capacity_evidence_v1";
  static constexpr std::string_view kSchemaV2 =
      "astra_book_capacity_evidence_v2";
  static constexpr std::string_view kSchema =
      kSchemaV2;

  BookCapacityProfile profile;
  std::string corpus_manifest_sha256;
  std::string profiler_sha256;
  // v2 binds the capacity values to the exact profiler stdout bytes. Empty
  // only when loading a backward-compatible v1 manifest.
  std::string profile_output_sha256;
  std::string schema;
};

BookCapacityEvidenceManifest loadBookCapacityEvidenceManifest(
    std::string_view path, std::string_view expected_profile_name,
    std::string_view expected_sha256);

struct BookCapacityValidation {
  BookCapacityHeadroom effective_headroom;
};

// Validates both the underlying storage configuration and the deployment
// evidence/reserve contract without mapping any arena. Throws before startup
// if profiled demand is not direct-indexed or monotonic page capacity lacks
// the declared reserve.
BookCapacityValidation
validateBookCapacityProfile(const BookCapacityProfile &profile);

// Final resident mapping sizes after alignment rounding. descriptor_slot_bytes
// records the logical C++ payload while descriptor_mapped_bytes records its
// padded, named VMA. Kernel bookkeeping outside these regions is excluded.
struct BookStorageFootprint {
  std::size_t system_page_bytes{0};
  std::size_t order_direct_mapped_bytes{0};
  std::size_t order_fallback_mapped_bytes{0};
  std::size_t price_roots_mapped_bytes{0};
  std::size_t price_prepared_books_mapped_bytes{0};
  std::size_t price_pages_mapped_bytes{0};
  std::size_t price_page_owners_mapped_bytes{0};
  std::size_t price_page_summaries_mapped_bytes{0};
  std::size_t price_page_occupancy_mapped_bytes{0};
  std::size_t price_book_summaries_mapped_bytes{0};
  std::size_t price_book_occupancy_mapped_bytes{0};
  std::size_t mapped_array_bytes{0};
  std::size_t descriptor_slot_bytes{0};
  std::size_t descriptor_mapped_bytes{0};
  std::size_t planned_storage_bytes{0};
};

// Cold end-of-session audit proving the one-directory-entry/one-book
// production contract. This is intentionally separate from the mutation hot
// path and scans the fixed Stock Locate domain only when requested.
struct BookDirectoryAudit {
  std::size_t registered_symbols{0};
  std::size_t materialized_books{0};
  std::uint32_t prepared_books{0};
  std::size_t registered_books_missing{0};
  std::size_t unregistered_books_present{0};
  std::size_t descriptor_price_state_mismatches{0};
  std::size_t descriptor_identity_mismatches{0};

  bool clean() const noexcept {
    return registered_books_missing == 0 &&
           unregistered_books_present == 0 &&
           descriptor_price_state_mismatches == 0 &&
           descriptor_identity_mismatches == 0 &&
           registered_symbols == materialized_books &&
           registered_symbols == prepared_books;
  }
};

// The explicit-page-size overload is deterministic and makes footprint tests
// independent of the host. Both overloads validate all size arithmetic before
// BookManager attempts any mmap or descriptor allocation.
BookStorageFootprint
estimateBookStorageFootprint(BookManagerConfig config,
                             std::size_t system_page_bytes);
BookStorageFootprint estimateBookStorageFootprint(BookManagerConfig config);

// Routes by direct Stock Locate while owning one process-wide order table and
// one process-wide full-domain price arena.  getOrCreate() is administrative;
// all message mutations require the descriptor to have been prepared already.
class BookManager {
public:
  static constexpr std::size_t kMaxStockLocate =
      astra::symbol::kStockLocateSlots;

  // Compact convenience construction is for tests/development. The live
  // md_engine always supplies a validated named deployment profile.
  BookManager();
  explicit BookManager(BookManagerConfig config);
  ~BookManager();

  OrderBook *getOrCreate(std::uint16_t stock_locate) noexcept;

  astra::book::MutationResult
  addOrder(std::uint16_t stock_locate, std::uint64_t order_id,
           std::uint64_t price, std::uint32_t qty, char side,
           std::uint64_t timestamp = 0) noexcept;
  astra::book::MutationResult
  cancelShares(std::uint16_t stock_locate, std::uint64_t order_id,
               std::uint32_t canceled_qty,
               std::uint64_t timestamp = 0) noexcept;
  astra::book::MutationResult
  deleteOrder(std::uint16_t stock_locate, std::uint64_t order_id,
              std::uint64_t timestamp = 0) noexcept;
  astra::book::MutationResult
  executeOrder(std::uint16_t stock_locate, std::uint64_t order_id,
               std::uint32_t executed_qty,
               std::uint64_t timestamp = 0) noexcept;
  bool trade(std::uint16_t stock_locate, std::uint64_t order_id,
             std::uint32_t executed_qty,
             std::uint64_t timestamp = 0) noexcept {
    return executeOrder(stock_locate, order_id, executed_qty, timestamp) ==
           astra::book::MutationResult::Applied;
  }
  astra::book::MutationResult
  replaceOrder(std::uint16_t stock_locate, std::uint64_t old_id,
               std::uint64_t new_id, std::uint64_t new_price,
               std::uint32_t new_qty,
               std::uint64_t timestamp = 0) noexcept;

  const OrderBook *getOrderBook(std::uint16_t stock_locate) const noexcept;
  // End-of-session validation only. This cold scan proves that Nasdaq's
  // end-of-day teardown mutations removed every displayed order before
  // System Event C is accepted.
  std::uint64_t totalLiveOrderCount() const noexcept;
  BookDirectoryAudit auditDirectory(
      const astra::symbol::StockDirectory &directory) const noexcept;

  const BookManagerConfig &config() const noexcept { return config_; }
  const BookStorageFootprint &storageFootprint() const noexcept {
    return storage_footprint_;
  }
  astra::book::OrderTable &orderTable() noexcept { return orders_; }
  const astra::book::OrderTable &orderTable() const noexcept {
    return orders_;
  }
  astra::book::PriceLevelStore &priceLevels() noexcept { return prices_; }
  const astra::book::PriceLevelStore &priceLevels() const noexcept {
    return prices_;
  }
  const void *bookDescriptorsArenaBase() const noexcept {
    return books_.data();
  }
  std::size_t bookDescriptorsArenaMappedBytes() const noexcept {
    return books_.mappedBytes();
  }

private:
  using BookSlot = std::optional<OrderBook>;

  OrderBook *find(std::uint16_t stock_locate) noexcept;
  const OrderBook *find(std::uint16_t stock_locate) const noexcept;

  BookManagerConfig config_;
  BookStorageFootprint storage_footprint_;
  astra::book::OrderTable orders_;
  astra::book::PriceLevelStore prices_;
  // Allocate every descriptor slot once during manager construction. A late
  // Stock Directory message can then activate its slot without calling the
  // heap allocator after Start of System Hours.
  astra::utils::MappedObjectArray<BookSlot> books_;
};
