#include "astra/book/BookManager.hpp"

#include <limits>
#include <stdexcept>
#include <sys/types.h>
#include <unistd.h>

namespace {

bool isPowerOfTwo(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

bool isLowerHexSha256(std::string_view value) noexcept {
  if (value.size() != 64)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::size_t checkedAdd(std::size_t lhs, std::size_t rhs) {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs)
    throw std::length_error("book storage size addition overflows size_t");
  return lhs + rhs;
}

std::size_t checkedMultiply(std::size_t lhs, std::size_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    throw std::length_error(
        "book storage size multiplication overflows size_t");
  }
  return lhs * rhs;
}

std::size_t checkedCount(std::uint64_t count) {
  if (count > std::numeric_limits<std::size_t>::max())
    throw std::length_error("book storage element count overflows size_t");
  return static_cast<std::size_t>(count);
}

std::size_t mappedBytes(std::size_t count, std::size_t element_bytes,
                        std::size_t element_alignment,
                        std::size_t requested_alignment,
                        std::size_t system_page_bytes) {
  const std::size_t logical_bytes = checkedMultiply(count, element_bytes);

  std::size_t alignment =
      requested_alignment == 0 ? system_page_bytes : requested_alignment;
  if (alignment < system_page_bytes)
    alignment = system_page_bytes;
  if (alignment < element_alignment)
    alignment = element_alignment;
  if (!isPowerOfTwo(alignment) ||
      alignment % system_page_bytes != 0) {
    throw std::invalid_argument(
        "book storage alignment must be a page-multiple power of two");
  }
  if (logical_bytes == 0)
    return 0;

  if (logical_bytes >
      std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
    throw std::length_error("book storage rounded size overflows size_t");
  }
  const std::size_t rounded_bytes =
      (logical_bytes + alignment - 1) & ~(alignment - 1);

  // MappedArray temporarily requests this extra alignment-sized region before
  // trimming its prefix and suffix. Validate that allocation arithmetic too,
  // while reporting only the final resident mapping below.
  if (rounded_bytes >
      std::numeric_limits<std::size_t>::max() - alignment) {
    throw std::length_error("book aligned mapping size overflows size_t");
  }
  return rounded_bytes;
}

void validateConfig(BookManagerConfig config) {
  if (config.orders.direct_slots == 0 ||
      config.orders.direct_slots >
          std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(
        "OrderTable direct_slots must fit nonzero size_t");
  }
  if (config.orders.fallback_bucket_count == 0 ||
      (config.orders.fallback_bucket_count &
       (config.orders.fallback_bucket_count - 1)) != 0) {
    throw std::invalid_argument(
        "OrderTable fallback_bucket_count must be a power of two");
  }
  if (config.prices.page_capacity == 0 ||
      config.prices.page_capacity ==
          std::numeric_limits<astra::book::PricePageHandle>::max()) {
    throw std::invalid_argument(
        "PriceLevelStore page capacity must be in [1, UINT32_MAX-1]");
  }
}

} // namespace

BookStorageFootprint
estimateBookStorageFootprint(BookManagerConfig config,
                             std::size_t system_page_bytes) {
  validateConfig(config);
  if (!isPowerOfTwo(system_page_bytes)) {
    throw std::invalid_argument(
        "system page size must be a nonzero power of two");
  }

  BookStorageFootprint footprint{};
  footprint.system_page_bytes = system_page_bytes;
  footprint.order_direct_mapped_bytes = mappedBytes(
      checkedCount(config.orders.direct_slots),
      sizeof(astra::book::OrderState), alignof(astra::book::OrderState),
      astra::book::OrderTable::kDirectStorageAlignment,
      system_page_bytes);
  footprint.order_fallback_mapped_bytes = mappedBytes(
      config.orders.fallback_bucket_count,
      astra::book::OrderTable::kFallbackBucketStorageBytes,
      astra::book::OrderTable::kFallbackBucketStorageAlignment,
      astra::book::OrderTable::kFallbackStorageAlignment,
      system_page_bytes);
  footprint.price_roots_mapped_bytes = mappedBytes(
      checkedCount(astra::book::kPriceRootSlotCount),
      sizeof(astra::book::PricePageHandle),
      alignof(astra::book::PricePageHandle),
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.price_prepared_books_mapped_bytes = mappedBytes(
      astra::symbol::kStockLocateSlots, sizeof(std::uint8_t),
      alignof(std::uint8_t),
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.price_pages_mapped_bytes = mappedBytes(
      config.prices.page_capacity, sizeof(astra::book::PricePage),
      alignof(astra::book::PricePage), astra::book::kPricePageBytes,
      system_page_bytes);

  const std::size_t page_metadata_slots =
      checkedAdd(config.prices.page_capacity, 1);
  footprint.price_page_owners_mapped_bytes = mappedBytes(
      page_metadata_slots,
      astra::book::PriceLevelStore::kPageOwnerStorageBytes,
      astra::book::PriceLevelStore::kPageOwnerStorageAlignment,
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.price_page_summaries_mapped_bytes = mappedBytes(
      page_metadata_slots,
      astra::book::PriceLevelStore::kPageSummaryStorageBytes,
      astra::book::PriceLevelStore::kPageSummaryStorageAlignment,
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.price_page_occupancy_mapped_bytes = mappedBytes(
      checkedMultiply(page_metadata_slots,
                      astra::book::PriceLevelStore::kSideCount),
      astra::book::PriceLevelStore::kBitmapStorageBytes,
      astra::book::PriceLevelStore::kBitmapStorageAlignment,
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.price_book_summaries_mapped_bytes = mappedBytes(
      astra::symbol::kStockLocateSlots,
      astra::book::PriceLevelStore::kBookSummaryStorageBytes,
      astra::book::PriceLevelStore::kBookSummaryStorageAlignment,
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.price_book_occupancy_mapped_bytes = mappedBytes(
      checkedMultiply(astra::symbol::kStockLocateSlots,
                      astra::book::PriceLevelStore::kSideCount),
      astra::book::PriceLevelStore::kBitmapStorageBytes,
      astra::book::PriceLevelStore::kBitmapStorageAlignment,
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);

  footprint.mapped_array_bytes = footprint.order_direct_mapped_bytes;
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes, footprint.order_fallback_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes, footprint.price_roots_mapped_bytes);
  footprint.mapped_array_bytes =
      checkedAdd(footprint.mapped_array_bytes,
                 footprint.price_prepared_books_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes, footprint.price_pages_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes, footprint.price_page_owners_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes,
      footprint.price_page_summaries_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes,
      footprint.price_page_occupancy_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes,
      footprint.price_book_summaries_mapped_bytes);
  footprint.mapped_array_bytes = checkedAdd(
      footprint.mapped_array_bytes,
      footprint.price_book_occupancy_mapped_bytes);

  footprint.descriptor_slot_bytes = checkedMultiply(
      BookManager::kMaxStockLocate, sizeof(std::optional<OrderBook>));
  footprint.descriptor_mapped_bytes = mappedBytes(
      BookManager::kMaxStockLocate, sizeof(std::optional<OrderBook>),
      alignof(std::optional<OrderBook>),
      astra::book::PriceLevelStore::kHotArenaAlignment,
      system_page_bytes);
  footprint.planned_storage_bytes =
      checkedAdd(footprint.mapped_array_bytes,
                 footprint.descriptor_mapped_bytes);
  return footprint;
}

BookStorageFootprint estimateBookStorageFootprint(BookManagerConfig config) {
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("unable to determine the system page size");
  return estimateBookStorageFootprint(config,
                                      static_cast<std::size_t>(page_size));
}

BookCapacityValidation
validateBookCapacityProfile(const BookCapacityProfile &profile) {
  // Reuse the exact constructor/storage validation before examining profile
  // arithmetic. This remains a read-only startup check and maps no memory.
  validateConfig(profile.config);

  if (profile.name.empty())
    throw std::invalid_argument("book capacity profile name must be nonempty");
  if (!isLowerHexSha256(profile.evidence_sha256)) {
    throw std::invalid_argument(
        "book capacity profile evidence SHA-256 must be 64 lowercase hex digits");
  }
  if (profile.evidence.profiled_max_order_reference == 0) {
    throw std::invalid_argument(
        "profiled maximum order reference must be nonzero");
  }
  if (profile.evidence.profiled_unique_price_pages == 0) {
    throw std::invalid_argument(
        "profiled unique price pages must be nonzero");
  }
  if (profile.minimum_headroom.direct_order_reference_slots == 0) {
    throw std::invalid_argument(
        "minimum direct-order-reference headroom must be nonzero");
  }
  if (profile.minimum_headroom.price_pages == 0) {
    throw std::invalid_argument(
        "minimum price-page headroom must be nonzero");
  }

  if (profile.evidence.profiled_max_order_reference >=
      profile.config.orders.direct_slots) {
    throw std::invalid_argument(
        "profiled maximum order reference is outside the direct table");
  }
  const std::uint64_t effective_direct_headroom =
      profile.config.orders.direct_slots -
      (profile.evidence.profiled_max_order_reference + 1);
  if (effective_direct_headroom <
      profile.minimum_headroom.direct_order_reference_slots) {
    throw std::invalid_argument(
        "direct order table does not provide the required profile headroom");
  }

  if (profile.evidence.profiled_unique_price_pages >=
      profile.config.prices.page_capacity) {
    throw std::invalid_argument(
        "profiled unique price pages leave no page-arena headroom");
  }
  const std::uint32_t effective_price_headroom =
      profile.config.prices.page_capacity -
      profile.evidence.profiled_unique_price_pages;
  if (effective_price_headroom < profile.minimum_headroom.price_pages) {
    throw std::invalid_argument(
        "price page arena does not provide the required profile headroom");
  }

  return {{effective_direct_headroom, effective_price_headroom}};
}

BookManager::BookManager() : BookManager(BookManagerConfig::compact()) {}

BookManager::BookManager(BookManagerConfig config)
    : config_(config),
      storage_footprint_(estimateBookStorageFootprint(config_)),
      orders_(config_.orders), prices_(config_.prices),
      books_(kMaxStockLocate,
             astra::book::PriceLevelStore::kHotArenaAlignment,
             "astra-book-descriptors",
             config_.orders.prefault || config_.prices.prefault) {}

BookManager::~BookManager() = default;

OrderBook *BookManager::find(std::uint16_t stock_locate) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return nullptr;
  BookSlot &slot = books_[stock_locate];
  return slot.has_value() ? &*slot : nullptr;
}

const OrderBook *
BookManager::find(std::uint16_t stock_locate) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return nullptr;
  const BookSlot &slot = books_[stock_locate];
  return slot.has_value() ? &*slot : nullptr;
}

OrderBook *BookManager::getOrCreate(std::uint16_t stock_locate) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return nullptr;
  if (OrderBook *book = find(stock_locate))
    return book;

  if (prices_.prepareBook(stock_locate) !=
      astra::book::MutationResult::Applied) {
    return nullptr;
  }
  const std::size_t reported_capacity =
      config_.orders.direct_slots >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::size_t>::max())
          ? std::numeric_limits<std::size_t>::max()
          : static_cast<std::size_t>(config_.orders.direct_slots);
  books_[stock_locate].emplace(stock_locate, orders_, prices_,
                               reported_capacity);
  return &*books_[stock_locate];
}

astra::book::MutationResult
BookManager::addOrder(std::uint16_t stock_locate, std::uint64_t order_id,
                      std::uint64_t price, std::uint32_t qty,
                      char side) noexcept {
  OrderBook *book = find(stock_locate);
  return book != nullptr
             ? book->addOrder(order_id, price, qty, side)
             : astra::book::MutationResult::BookNotPrepared;
}

astra::book::MutationResult BookManager::cancelShares(
    std::uint16_t stock_locate, std::uint64_t order_id,
    std::uint32_t canceled_qty) noexcept {
  OrderBook *book = find(stock_locate);
  return book != nullptr
             ? book->cancelShares(order_id, canceled_qty)
             : astra::book::MutationResult::BookNotPrepared;
}

astra::book::MutationResult
BookManager::deleteOrder(std::uint16_t stock_locate,
                         std::uint64_t order_id) noexcept {
  OrderBook *book = find(stock_locate);
  return book != nullptr ? book->deleteOrder(order_id)
                         : astra::book::MutationResult::BookNotPrepared;
}

astra::book::MutationResult BookManager::executeOrder(
    std::uint16_t stock_locate, std::uint64_t order_id,
    std::uint32_t executed_qty) noexcept {
  OrderBook *book = find(stock_locate);
  return book != nullptr
             ? book->executeOrder(order_id, executed_qty)
             : astra::book::MutationResult::BookNotPrepared;
}

astra::book::MutationResult BookManager::replaceOrder(
    std::uint16_t stock_locate, std::uint64_t old_id,
    std::uint64_t new_id, std::uint64_t new_price,
    std::uint32_t new_qty) noexcept {
  OrderBook *book = find(stock_locate);
  return book != nullptr
             ? book->replaceOrder(old_id, new_id, new_price, new_qty)
             : astra::book::MutationResult::BookNotPrepared;
}

const OrderBook *
BookManager::getOrderBook(std::uint16_t stock_locate) const noexcept {
  return find(stock_locate);
}
