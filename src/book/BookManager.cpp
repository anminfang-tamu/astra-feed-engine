#include "astra/book/BookManager.hpp"

#include <algorithm>
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

size_t validateOrderPoolCapacity(size_t directory_slots,
                                 size_t order_pool_capacity) {
  const size_t directory_max_entries = directory_slots / 2;
  if (order_pool_capacity == 0 ||
      order_pool_capacity > directory_max_entries ||
      order_pool_capacity > OrderArena::kInvalidIndex) {
    throw std::invalid_argument(
        "order-pool capacity must be nonzero and no larger than the order "
        "directory's half-load entry limit");
  }
  return order_pool_capacity;
}

} // namespace

BookManager::BookManager(size_t order_directory_slots)
    : BookManager(order_directory_slots,
                  defaultPriceLevelArenaConfig(order_directory_slots / 2),
                  order_directory_slots / 2) {}

BookManager::BookManager(size_t order_directory_slots,
                         PriceLevelArenaConfig price_level_config)
    : BookManager(order_directory_slots, price_level_config,
                  order_directory_slots / 2) {}

BookManager::BookManager(size_t order_directory_slots,
                         PriceLevelArenaConfig price_level_config,
                         size_t order_pool_capacity)
    : order_refs_(order_directory_slots),
      orders_(validateOrderPoolCapacity(order_directory_slots,
                                        order_pool_capacity)),
      price_levels_(validatePriceLevelConfig(price_level_config)) {}

BookManager::~BookManager() = default;

PriceLevelArenaConfig BookManager::defaultPriceLevelArenaConfig(
    size_t order_pool_capacity) noexcept {
  const size_t max_orders = order_pool_capacity;
  const size_t desired_nodes =
      max_orders > kDefaultPriceInternalNodeCapacity / 2
          ? kDefaultPriceInternalNodeCapacity
          : std::max(size_t{2}, max_orders * 2);
  return {
      std::min(desired_nodes, kDefaultPriceInternalNodeCapacity),
      std::max(size_t{1},
               std::min(max_orders, kDefaultPriceLeafCapacity)),
      std::max(size_t{1},
               std::min(max_orders, kDefaultPriceLevelCapacity)),
  };
}

OrderBook *BookManager::find(uint16_t stock_locate) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return nullptr;
  return books_[stock_locate].get();
}

OrderBook *BookManager::getOrCreate(uint16_t stock_locate) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return nullptr;
  }

  if (books_[stock_locate] == nullptr) {
    if (book_universe_sealed_) {
      ++late_book_creation_attempts_;
      return nullptr;
    }
    books_[stock_locate] =
        std::make_unique<OrderBook>(stock_locate, orders_, price_levels_);
  }

  return books_[stock_locate].get();
}

bool BookManager::eraseStaleOrderRef(
    uint64_t order_id, OrderRefDirectory::Handle handle) noexcept {
  if (!handle.valid()) {
    return false;
  }

  OrderBook *book = find(handle.stock_locate);
  if (book != nullptr && book->hasOrderAt(handle.pool_index, order_id)) {
    return false;
  }

  if (!order_refs_.erase(order_id)) {
    ++directory_erase_failures_;
  }
  ++stale_order_refs_;
  if (book != nullptr) {
    recordMutationFailure(*book);
  } else {
    ++mutation_failures_;
  }
  return true;
}

OrderBook *
BookManager::resolveOrderRef(uint16_t stock_locate, uint64_t order_id,
                             OrderRefDirectory::Handle &handle) noexcept {
  handle = order_refs_.find(order_id);
  if (!handle.valid()) {
    ++missing_order_refs_;
    return nullptr;
  }

  if (handle.stock_locate != stock_locate) {
    ++missing_order_refs_;
    if (OrderBook *referenced_book = find(handle.stock_locate)) {
      recordMutationFailure(*referenced_book);
    } else {
      ++mutation_failures_;
    }
    return nullptr;
  }

  OrderBook *book = find(handle.stock_locate);
  if (book == nullptr || !book->hasOrderAt(handle.pool_index, order_id)) {
    if (!order_refs_.erase(order_id)) {
      ++directory_erase_failures_;
    }
    ++stale_order_refs_;
    ++missing_order_refs_;
    if (book != nullptr) {
      recordMutationFailure(*book);
    } else {
      ++mutation_failures_;
    }
    handle = {};
    return nullptr;
  }

  return book;
}

bool BookManager::eraseOrderRef(uint64_t order_id, OrderBook &book) noexcept {
  if (order_refs_.erase(order_id)) {
    return true;
  }
  ++directory_erase_failures_;
  recordMutationFailure(book);
  return false;
}

void BookManager::recordMutationFailure(OrderBook &book) noexcept {
  ++mutation_failures_;
  book.markLocalInvalid();
}

void BookManager::addOrder(uint16_t stock_locate, uint64_t order_id,
                           uint64_t price, uint32_t qty, char side) noexcept {
  if (!isValidAdd(stock_locate, order_id, qty, side)) {
    ++invalid_adds_;
    return;
  }

  const OrderRefDirectory::Handle existing = order_refs_.find(order_id);
  if (existing.valid()) {
    if (eraseStaleOrderRef(order_id, existing)) {
      // A stale authoritative reference is a consistency failure. Do not
      // recover by accepting the new order into a now-invalid book.
      return;
    }
    ++duplicate_order_refs_;
    if (OrderBook *existing_book = find(existing.stock_locate)) {
      recordMutationFailure(*existing_book);
    } else {
      ++mutation_failures_;
    }
    return;
  }

  if (order_refs_.size() >= order_refs_.maxEntries()) {
    ++directory_insert_failures_;
    if (OrderBook *book = getOrCreate(stock_locate)) {
      recordMutationFailure(*book);
    } else {
      ++mutation_failures_;
    }
    return;
  }

  OrderBook *book = getOrCreate(stock_locate);
  if (book == nullptr) {
    ++invalid_adds_;
    return;
  }

  const uint32_t order_idx = book->addOrderIndexed(order_id, price, qty, side);
  if (order_idx == OrderBook::kInvalidIdx) {
    return;
  }

  const OrderRefDirectory::InsertResult insert_result =
      order_refs_.insert(order_id, stock_locate, order_idx);
  if (insert_result == OrderRefDirectory::InsertResult::Inserted) {
    return;
  }

  ++directory_insert_failures_;
  if (insert_result == OrderRefDirectory::InsertResult::Duplicate) {
    ++duplicate_order_refs_;
  }
  (void)book->deleteOrderIndexed(order_idx, order_id);
  recordMutationFailure(*book);
}

void BookManager::cancelShares(uint16_t stock_locate, uint64_t order_id,
                               uint32_t canceled_qty) noexcept {
  OrderRefDirectory::Handle handle{};
  OrderBook *book = resolveOrderRef(stock_locate, order_id, handle);
  if (book == nullptr) {
    return;
  }

  const OrderBook::MutationResult result =
      book->cancelSharesIndexed(handle.pool_index, order_id, canceled_qty);
  if (result == OrderBook::MutationResult::Removed) {
    (void)eraseOrderRef(order_id, *book);
  } else if (result == OrderBook::MutationResult::Ignored) {
    recordMutationFailure(*book);
  }
}

void BookManager::deleteOrder(uint16_t stock_locate,
                              uint64_t order_id) noexcept {
  OrderRefDirectory::Handle handle{};
  OrderBook *book = resolveOrderRef(stock_locate, order_id, handle);
  if (book == nullptr) {
    return;
  }

  if (book->deleteOrderIndexed(handle.pool_index, order_id)) {
    (void)eraseOrderRef(order_id, *book);
  } else {
    recordMutationFailure(*book);
  }
}

bool BookManager::trade(uint16_t stock_locate, uint64_t order_id,
                        uint32_t executed_qty) noexcept {
  OrderRefDirectory::Handle handle{};
  OrderBook *book = resolveOrderRef(stock_locate, order_id, handle);
  if (book == nullptr) {
    return false;
  }

  const OrderBook::MutationResult result =
      book->tradeIndexed(handle.pool_index, order_id, executed_qty);
  if (result == OrderBook::MutationResult::Removed) {
    (void)eraseOrderRef(order_id, *book);
  } else if (result == OrderBook::MutationResult::Ignored) {
    recordMutationFailure(*book);
  }
  return result != OrderBook::MutationResult::Ignored;
}

void BookManager::replaceOrder(uint16_t stock_locate, uint64_t old_id,
                               uint64_t new_id, uint64_t new_price,
                               uint32_t new_qty) noexcept {
  OrderRefDirectory::Handle handle{};
  OrderBook *book = resolveOrderRef(stock_locate, old_id, handle);
  if (book == nullptr) {
    return;
  }

  if (new_id != old_id) {
    const OrderRefDirectory::Handle existing = order_refs_.find(new_id);
    if (existing.valid()) {
      if (eraseStaleOrderRef(new_id, existing)) {
        return;
      }
      ++duplicate_order_refs_;
      recordMutationFailure(*book);
      return;
    }
  }

  const OrderBook::MutationResult result = book->replaceOrderIndexed(
      handle.pool_index, old_id, new_id, new_price, new_qty);
  if (result == OrderBook::MutationResult::Ignored) {
    recordMutationFailure(*book);
    return;
  }
  if (result == OrderBook::MutationResult::Removed) {
    (void)eraseOrderRef(old_id, *book);
    return;
  }

  const OrderRefDirectory::ReplaceResult replace_result =
      order_refs_.replaceKey(old_id, new_id);
  if (replace_result == OrderRefDirectory::ReplaceResult::Replaced) {
    return;
  }

  ++directory_replace_failures_;
  (void)book->deleteOrderIndexed(handle.pool_index, new_id);
  if (!order_refs_.erase(old_id)) {
    ++directory_erase_failures_;
  }
  recordMutationFailure(*book);
}

void BookManager::reverseExecution(uint16_t stock_locate, uint64_t order_id,
                                   uint64_t price, uint32_t qty,
                                   char side) noexcept {
  if (qty == 0) {
    return;
  }

  const OrderRefDirectory::Handle handle = order_refs_.find(order_id);
  if (handle.valid()) {
    OrderBook *book = find(handle.stock_locate);
    if (handle.stock_locate == stock_locate && book != nullptr &&
        book->restoreSharesIndexed(handle.pool_index, order_id, qty) ==
            OrderBook::MutationResult::Updated) {
      return;
    }
    if (book != nullptr) {
      recordMutationFailure(*book);
    } else {
      ++mutation_failures_;
    }
    return;
  }

  addOrder(stock_locate, order_id, price, qty, side);
}

const OrderBook *
BookManager::getOrderBook(uint16_t stock_locate) const noexcept {
  return find(stock_locate);
}

const Order *BookManager::getOrder(uint64_t order_id) const noexcept {
  const OrderRefDirectory::Handle handle = order_refs_.find(order_id);
  if (!handle.valid()) {
    return nullptr;
  }
  const OrderBook *book = find(handle.stock_locate);
  if (book == nullptr || !book->hasOrderAt(handle.pool_index, order_id)) {
    return nullptr;
  }
  return book->orderAt(handle.pool_index);
}

BookManagerStats BookManager::stats() const noexcept {
  BookManagerStats result{};
  result.invalid_adds = invalid_adds_;
  result.duplicate_order_refs = duplicate_order_refs_;
  result.missing_order_refs = missing_order_refs_;
  result.stale_order_refs = stale_order_refs_;
  result.mutation_failures = mutation_failures_;
  result.directory_insert_failures = directory_insert_failures_;
  result.directory_replace_failures = directory_replace_failures_;
  result.directory_erase_failures = directory_erase_failures_;
  result.late_book_creation_attempts = late_book_creation_attempts_;
  result.directory_size = order_refs_.size();
  result.directory_capacity = order_refs_.capacity();
  result.directory_max_entries = order_refs_.maxEntries();
  result.directory_max_probe_length = order_refs_.maxProbeLength();
  result.orders = orders_.stats();
  result.price_levels = price_levels_.stats();

  for (const std::unique_ptr<OrderBook> &book : books_) {
    if (book == nullptr) {
      continue;
    }
    ++result.books;
    result.live_orders += book->liveOrderCount();
    result.price_rejections += book->priceRejections();
    if (book->localInvalid()) {
      ++result.locally_invalid_books;
    }
  }

  result.allocation_failures = result.orders.allocation_failures;

  return result;
}
