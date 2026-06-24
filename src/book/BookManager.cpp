#include "astra/book/BookManager.hpp"

namespace {

constexpr uint64_t kInvalidOrderId = 0;

bool isValidSide(char side) noexcept { return side == 'B' || side == 'S'; }

bool isValidAdd(uint16_t stock_locate, uint64_t order_id, uint32_t qty,
                char side) noexcept {
  return astra::symbol::isValidStockLocate(stock_locate) &&
         order_id != kInvalidOrderId && qty != 0 && isValidSide(side);
}

} // namespace

BookManager::BookManager() = default;

BookManager::~BookManager() = default;

OrderBook *BookManager::find(uint16_t stock_locate) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return nullptr;
  return books_[stock_locate].get();
}

size_t BookManager::orderCapacityForLocate(uint16_t stock_locate) const
    noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return astra::book_capacity::kDefaultOrderCapacity;
  }
  const size_t configured_capacity = order_capacity_by_locate_[stock_locate];
  return configured_capacity != 0 ? configured_capacity
                                  : astra::book_capacity::kDefaultOrderCapacity;
}

OrderBook *BookManager::getOrCreate(uint16_t stock_locate) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return nullptr;
  }

  if (books_[stock_locate] == nullptr) {
    books_[stock_locate] = std::make_unique<OrderBook>(
        stock_locate, orderCapacityForLocate(stock_locate));
  }

  return books_[stock_locate].get();
}

bool BookManager::removeDirectIfStale(
    uint64_t order_id, OrderRefDirectory::Handle handle) noexcept {
  if (!handle.valid()) {
    return false;
  }

  OrderBook *book = find(handle.stock_locate);
  if (book != nullptr && book->hasOrderAt(handle.pool_index, order_id)) {
    return false;
  }

  order_refs_.erase(order_id);
  return true;
}

OrderBook *
BookManager::resolveDirect(uint16_t stock_locate, uint64_t order_id,
                           OrderRefDirectory::Handle &handle) noexcept {
  handle = order_refs_.find(order_id);
  if (!handle.valid()) {
    return nullptr;
  }

  if (handle.stock_locate != stock_locate) {
    return nullptr;
  }

  OrderBook *book = find(handle.stock_locate);
  if (book == nullptr || !book->hasOrderAt(handle.pool_index, order_id)) {
    order_refs_.erase(order_id);
    handle = {};
    return nullptr;
  }

  return book;
}

void BookManager::setBookCapacityTier(uint16_t stock_locate,
                                      astra::symbol::SymbolTier tier) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    return;
  }
  if (books_[stock_locate] != nullptr) {
    return;
  }
  order_capacity_by_locate_[stock_locate] =
      astra::book_capacity::orderCapacity(tier);
}

void BookManager::addOrder(uint16_t stock_locate, uint64_t order_id,
                           uint64_t price, uint32_t qty, char side) noexcept {
  if (!isValidAdd(stock_locate, order_id, qty, side)) {
    return;
  }

  if (!order_refs_.canStore(order_id)) {
    OrderBook *book = getOrCreate(stock_locate);
    if (book != nullptr) {
      book->addOrder(order_id, price, qty, side);
    }
    return;
  }

  const OrderRefDirectory::Handle existing = order_refs_.find(order_id);
  if (existing.valid() && !removeDirectIfStale(order_id, existing)) {
    return;
  }

  OrderBook *book = getOrCreate(stock_locate);
  if (book == nullptr) {
    return;
  }

  const uint32_t order_idx = book->addOrderIndexed(order_id, price, qty, side);
  if (order_idx == OrderBook::kInvalidIdx) {
    return;
  }

  if (!order_refs_.insert(order_id, stock_locate, order_idx)) {
    (void)book->deleteOrderIndexed(order_idx, order_id, false);
  }
}

void BookManager::cancelShares(uint16_t stock_locate, uint64_t order_id,
                               uint32_t canceled_qty) noexcept {
  OrderRefDirectory::Handle handle{};
  if (OrderBook *book = resolveDirect(stock_locate, order_id, handle)) {
    const OrderBook::MutationResult result =
        book->cancelSharesIndexed(handle.pool_index, order_id, canceled_qty,
                                  false);
    if (result == OrderBook::MutationResult::Removed) {
      order_refs_.erase(order_id);
    }
    return;
  }

  OrderBook *book = find(stock_locate);
  if (book != nullptr) {
    book->cancelShares(order_id, canceled_qty);
  }
}

void BookManager::deleteOrder(uint16_t stock_locate,
                              uint64_t order_id) noexcept {
  OrderRefDirectory::Handle handle{};
  if (OrderBook *book = resolveDirect(stock_locate, order_id, handle)) {
    if (book->deleteOrderIndexed(handle.pool_index, order_id, false)) {
      order_refs_.erase(order_id);
    }
    return;
  }

  OrderBook *book = find(stock_locate);
  if (book != nullptr) {
    book->deleteOrder(order_id);
  }
}

bool BookManager::trade(uint16_t stock_locate, uint64_t order_id,
                        uint32_t executed_qty) noexcept {
  OrderRefDirectory::Handle handle{};
  if (OrderBook *book = resolveDirect(stock_locate, order_id, handle)) {
    const OrderBook::MutationResult result =
        book->tradeIndexed(handle.pool_index, order_id, executed_qty, false);
    if (result == OrderBook::MutationResult::Removed) {
      order_refs_.erase(order_id);
    }
    return result != OrderBook::MutationResult::Ignored;
  }

  OrderBook *book = find(stock_locate);
  return book != nullptr && book->trade(order_id, executed_qty);
}

void BookManager::replaceOrder(uint16_t stock_locate, uint64_t old_id,
                               uint64_t new_id, uint64_t new_price,
                               uint32_t new_qty) noexcept {
  OrderRefDirectory::Handle handle{};
  if (OrderBook *book = resolveDirect(stock_locate, old_id, handle)) {
    if (new_id != old_id && order_refs_.canStore(new_id)) {
      const OrderRefDirectory::Handle existing = order_refs_.find(new_id);
      if (existing.valid() && !removeDirectIfStale(new_id, existing)) {
        if (book->deleteOrderIndexed(handle.pool_index, old_id, false)) {
          order_refs_.erase(old_id);
        }
        return;
      }
    }

    const OrderBook::MutationResult result = book->replaceOrderIndexed(
        handle.pool_index, old_id, new_id, new_price, new_qty, false);
    if (result == OrderBook::MutationResult::Ignored) {
      return;
    }

    order_refs_.erase(old_id);
    if (result == OrderBook::MutationResult::Removed) {
      return;
    }

    if (order_refs_.canStore(new_id)) {
      if (!order_refs_.insert(new_id, stock_locate, handle.pool_index)) {
        (void)book->deleteOrderIndexed(handle.pool_index, new_id, false);
      }
    } else if (!book->registerOrderIndex(new_id, handle.pool_index)) {
      (void)book->deleteOrderIndexed(handle.pool_index, new_id, false);
    }
    return;
  }

  OrderBook *book = find(stock_locate);
  if (book != nullptr) {
    book->replaceOrder(old_id, new_id, new_price, new_qty);
  }
}

void BookManager::reverseExecution(uint16_t stock_locate, uint64_t order_id,
                                   uint64_t price, uint32_t qty,
                                   char side) noexcept {
  OrderRefDirectory::Handle handle{};
  if (OrderBook *book = resolveDirect(stock_locate, order_id, handle)) {
    (void)book->restoreSharesIndexed(handle.pool_index, order_id, qty);
    return;
  }

  if (order_refs_.canStore(order_id)) {
    addOrder(stock_locate, order_id, price, qty, side);
    return;
  }

  OrderBook *book = find(stock_locate);
  if (book != nullptr) {
    book->reverseExecution(order_id, price, qty, side);
  }
}

const OrderBook *
BookManager::getOrderBook(uint16_t stock_locate) const noexcept {
  return find(stock_locate);
}
