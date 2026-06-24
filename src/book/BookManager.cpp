#include "astra/book/BookManager.hpp"

BookManager::BookManager() = default;

BookManager::~BookManager() = default;

OrderBook *BookManager::find(uint16_t stock_locate) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return nullptr;
  return books_[stock_locate].get();
}

// size_t
// BookManager::orderCapacityForLocate(uint16_t stock_locate) const noexcept {
//   if (!astra::symbol::isValidStockLocate(stock_locate)) {
//     return astra::book_capacity::kDefaultOrderCapacity;
//   }
//   const size_t configured_capacity = order_capacity_by_locate_[stock_locate];
//   return configured_capacity != 0 ? configured_capacity
//                                   :
//                                   astra::book_capacity::kDefaultOrderCapacity;
// }

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
  OrderBook *book = find(stock_locate);
  if (book)
    book->addOrder(order_id, price, qty, side);
}

void BookManager::cancelShares(uint16_t stock_locate, uint64_t order_id,
                               uint32_t canceled_qty) noexcept {
  OrderBook *book = find(stock_locate);
  if (book)
    book->cancelShares(order_id, canceled_qty);
}

void BookManager::deleteOrder(uint16_t stock_locate,
                              uint64_t order_id) noexcept {
  OrderBook *book = find(stock_locate);
  if (book)
    book->deleteOrder(order_id);
}

bool BookManager::trade(uint16_t stock_locate, uint64_t order_id,
                        uint32_t executed_qty) noexcept {
  OrderBook *book = find(stock_locate);
  if (book)
    return book->trade(order_id, executed_qty);
  return false;
}

void BookManager::replaceOrder(uint16_t stock_locate, uint64_t old_id,
                               uint64_t new_id, uint64_t new_price,
                               uint32_t new_qty) noexcept {
  OrderBook *book = find(stock_locate);
  if (book)
    book->replaceOrder(old_id, new_id, new_price, new_qty);
}

void BookManager::reverseExecution(uint16_t stock_locate, uint64_t order_id,
                                   uint64_t price, uint32_t qty,
                                   char side) noexcept {
  OrderBook *book = find(stock_locate);
  if (book)
    book->reverseExecution(order_id, price, qty, side);
}

const OrderBook *
BookManager::getOrderBook(uint16_t stock_locate) const noexcept {
  return find(stock_locate);
}
