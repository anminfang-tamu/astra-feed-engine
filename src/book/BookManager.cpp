#include "astra/book/BookManager.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace {

constexpr uint64_t kInvalidOrderId = 0;

bool isValidSide(char side) noexcept { return side == 'B' || side == 'S'; }

bool isValidAdd(uint16_t stock_locate, uint64_t order_id, uint32_t qty,
                char side) noexcept {
  return astra::symbol::isValidStockLocate(stock_locate) &&
         order_id != kInvalidOrderId && qty != 0 && isValidSide(side);
}

PriceLevelArenaConfig
validatePriceLevelConfig(PriceLevelArenaConfig config) {
  if (config.internal_node_capacity < 2 || config.leaf_capacity == 0 ||
      config.level_capacity == 0 ||
      config.level_capacity > Order::kLevelMask) {
    throw std::invalid_argument(
        "price-level capacities must be nonzero, provide at least two "
        "internal nodes, and fit the packed Order level handle");
  }
  return config;
}

size_t validateOrderCapacity(size_t capacity) {
  if (capacity == 0 ||
      capacity > static_cast<size_t>(OrderArena::kInvalidIndex)) {
    throw std::invalid_argument(
        "per-book order capacity must fit the uint32 local index domain");
  }
  return capacity;
}

void addOrderArenaStats(OrderArenaStats &aggregate,
                        const OrderArenaStats &book) noexcept {
  aggregate.capacity += book.capacity;
  aggregate.in_use += book.in_use;
  aggregate.allocation_failures += book.allocation_failures;
  aggregate.invalid_releases += book.invalid_releases;
  aggregate.double_releases += book.double_releases;
}

} // namespace

BookManager::BookManager()
    : BookManager(astra::book_capacity::kDefaultOrderCapacity,
                  {kDefaultPriceInternalNodeCapacity,
                   kDefaultPriceLeafCapacity,
                   kDefaultPriceLevelCapacity}) {}

BookManager::BookManager(size_t default_order_capacity,
                         PriceLevelArenaConfig price_level_config)
    : default_order_capacity_(validateOrderCapacity(default_order_capacity)),
      price_levels_(validatePriceLevelConfig(price_level_config)) {}

BookManager::~BookManager() = default;

OrderBook *BookManager::find(uint16_t stock_locate) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return nullptr;
  }
  return books_[stock_locate].get();
}

size_t
BookManager::orderCapacityForLocate(uint16_t stock_locate) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return default_order_capacity_;
  }
  const size_t configured = order_capacity_by_locate_[stock_locate];
  return configured == 0 ? default_order_capacity_ : configured;
}

void BookManager::setBookOrderCapacity(uint16_t stock_locate,
                                       size_t order_capacity) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate) ||
      book_universe_sealed_ || books_[stock_locate] != nullptr ||
      order_capacity == 0 ||
      order_capacity > static_cast<size_t>(OrderArena::kInvalidIndex)) {
    return;
  }
  order_capacity_by_locate_[stock_locate] = order_capacity;
  explicit_order_capacity_by_locate_[stock_locate] = true;
}

void BookManager::setBookCapacityTier(
    uint16_t stock_locate, astra::symbol::SymbolTier tier) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate) ||
      book_universe_sealed_ || books_[stock_locate] != nullptr ||
      explicit_order_capacity_by_locate_[stock_locate]) {
    return;
  }
  order_capacity_by_locate_[stock_locate] =
      tier == astra::symbol::SymbolTier::Default
          ? 0
          : astra::book_capacity::orderCapacity(tier);
}

OrderBook *BookManager::getOrCreate(uint16_t stock_locate) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return nullptr;
  }
  if (books_[stock_locate] != nullptr) {
    return books_[stock_locate].get();
  }
  if (book_universe_sealed_) {
    ++late_book_creation_attempts_;
    return nullptr;
  }

  try {
    books_[stock_locate] = std::make_unique<OrderBook>(
        stock_locate, orderCapacityForLocate(stock_locate), price_levels_);
  } catch (const std::bad_alloc &) {
    ++book_creation_failures_;
    return nullptr;
  } catch (const std::length_error &) {
    ++book_creation_failures_;
    return nullptr;
  } catch (const std::invalid_argument &) {
    ++book_creation_failures_;
    return nullptr;
  }
  return books_[stock_locate].get();
}

bool BookManager::prepareBook(uint16_t stock_locate) noexcept {
  return getOrCreate(stock_locate) != nullptr;
}

void BookManager::observeLiveOrderChange(size_t before,
                                         size_t after) noexcept {
  if (after >= before) {
    live_orders_ += after - before;
  } else {
    const size_t removed = before - after;
    live_orders_ = removed <= live_orders_ ? live_orders_ - removed : 0;
  }
  live_order_high_watermark_ =
      std::max(live_order_high_watermark_, live_orders_);
}

void BookManager::addOrder(uint16_t stock_locate, uint64_t order_id,
                           uint64_t price, uint32_t qty, char side) noexcept {
  if (!isValidAdd(stock_locate, order_id, qty, side)) {
    ++invalid_adds_;
    return;
  }
  OrderBook *book = getOrCreate(stock_locate);
  if (book == nullptr) {
    ++invalid_adds_;
    return;
  }
  const size_t before = book->liveOrderCount();
  book->addOrder(order_id, price, qty, side);
  observeLiveOrderChange(before, book->liveOrderCount());
}

void BookManager::cancelShares(uint16_t stock_locate, uint64_t order_id,
                               uint32_t canceled_qty) noexcept {
  OrderBook *book = find(stock_locate);
  if (book == nullptr) {
    ++missing_order_refs_;
    return;
  }
  const size_t before = book->liveOrderCount();
  book->cancelShares(order_id, canceled_qty);
  observeLiveOrderChange(before, book->liveOrderCount());
}

void BookManager::deleteOrder(uint16_t stock_locate,
                              uint64_t order_id) noexcept {
  OrderBook *book = find(stock_locate);
  if (book == nullptr) {
    ++missing_order_refs_;
    return;
  }
  const size_t before = book->liveOrderCount();
  book->deleteOrder(order_id);
  observeLiveOrderChange(before, book->liveOrderCount());
}

bool BookManager::trade(uint16_t stock_locate, uint64_t order_id,
                        uint32_t executed_qty) noexcept {
  OrderBook *book = find(stock_locate);
  if (book == nullptr) {
    ++missing_order_refs_;
    return false;
  }
  const size_t before = book->liveOrderCount();
  const bool applied = book->trade(order_id, executed_qty);
  observeLiveOrderChange(before, book->liveOrderCount());
  return applied;
}

void BookManager::replaceOrder(uint16_t stock_locate, uint64_t old_id,
                               uint64_t new_id, uint64_t new_price,
                               uint32_t new_qty) noexcept {
  OrderBook *book = find(stock_locate);
  if (book == nullptr) {
    ++missing_order_refs_;
    return;
  }
  const size_t before = book->liveOrderCount();
  book->replaceOrder(old_id, new_id, new_price, new_qty);
  observeLiveOrderChange(before, book->liveOrderCount());
}

void BookManager::reverseExecution(uint16_t stock_locate, uint64_t order_id,
                                   uint64_t price, uint32_t qty,
                                   char side) noexcept {
  if (!isValidAdd(stock_locate, order_id, qty, side) ||
      price > std::numeric_limits<uint32_t>::max()) {
    ++mutation_failures_;
    return;
  }
  OrderBook *book = find(stock_locate);
  if (book == nullptr) {
    book = getOrCreate(stock_locate);
  }
  if (book == nullptr) {
    ++missing_order_refs_;
    return;
  }
  const size_t before = book->liveOrderCount();
  book->reverseExecution(order_id, price, qty, side);
  observeLiveOrderChange(before, book->liveOrderCount());
}

const OrderBook *
BookManager::getOrderBook(uint16_t stock_locate) const noexcept {
  return find(stock_locate);
}

const Order *BookManager::getOrder(uint16_t stock_locate,
                                   uint64_t order_id) const noexcept {
  const OrderBook *book = find(stock_locate);
  return book == nullptr ? nullptr : book->getOrder(order_id);
}

BookManagerStats BookManager::stats() const noexcept {
  return stats(BookStatsDetail::Full);
}

BookManagerStats BookManager::stats(BookStatsDetail detail) const noexcept {
  BookManagerStats result{};
  result.invalid_adds = invalid_adds_;
  result.missing_order_refs = missing_order_refs_;
  result.mutation_failures = mutation_failures_;
  result.late_book_creation_attempts = late_book_creation_attempts_;
  result.book_creation_failures = book_creation_failures_;
  result.live_order_high_watermark = live_order_high_watermark_;
  result.price_levels = price_levels_.stats();

  size_t summed_live_orders = 0;
  for (const std::unique_ptr<OrderBook> &book : books_) {
    if (book == nullptr) {
      continue;
    }
    ++result.books;
    const OrderBookStats book_stats = book->stats(detail);
    result.invalid_adds += book_stats.invalid_adds;
    result.duplicate_order_refs += book_stats.duplicate_order_refs;
    result.missing_order_refs += book_stats.missing_order_refs;
    result.stale_order_refs += book_stats.stale_order_refs;
    result.mutation_failures += book_stats.mutation_failures;
    result.order_ref_index_insert_failures +=
        book_stats.index_insert_failures;
    result.order_ref_index_replace_failures +=
        book_stats.index_replace_failures;
    result.order_ref_index_erase_failures += book_stats.index_erase_failures;
    result.price_rejections += book_stats.price_rejections;
    result.order_ref_index_size += book_stats.index_size;
    result.order_ref_index_capacity += book_stats.index_capacity;
    result.order_ref_index_max_entries += book_stats.index_max_entries;
    result.order_ref_index_max_probe_length = std::max(
        result.order_ref_index_max_probe_length,
        book_stats.index_probes.max_length);
    addOrderArenaStats(result.orders, book_stats.orders);
    summed_live_orders += book_stats.live_orders;
    if (book->localInvalid()) {
      ++result.locally_invalid_books;
    }
    if (book_stats.index_size != book_stats.live_orders ||
        book_stats.orders.in_use != book_stats.live_orders) {
      ++result.inconsistent_books;
    }
  }

  result.live_orders = summed_live_orders;
  result.orders.high_watermark = live_order_high_watermark_;
  result.allocation_failures = result.orders.allocation_failures;
  if (summed_live_orders != live_orders_) {
    ++result.inconsistent_books;
  }
  return result;
}
