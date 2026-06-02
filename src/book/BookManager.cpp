#include "astra/book/BookManager.hpp"
#include "astra/utils/DebugTrace.hpp"

BookManager::BookManager() = default;

BookManager::~BookManager() = default;

OrderBook *BookManager::find(uint16_t stock_locate) const noexcept {
  if (stock_locate == 0 || stock_locate >= kMaxStockLocate) return nullptr;
  return book_by_locate_[stock_locate];
}

OrderBook *BookManager::getOrCreate(uint16_t stock_locate) noexcept {
  return getOrCreate(stock_locate, 0);
}

OrderBook *BookManager::getOrCreate(uint16_t stock_locate,
                                    uint16_t channel_id) noexcept {
  ASTRA_TRACE("book_manager getOrCreate locate=%u channel=%u",
              stock_locate, channel_id);
  if (stock_locate == 0 || stock_locate >= kMaxStockLocate) return nullptr;
  OrderBook *book = book_by_locate_[stock_locate];
  if (book) {
    ASTRA_TRACE("book_manager found existing locate=%u book=%p", stock_locate,
                static_cast<void *>(book));
    book->setChannelId(channel_id);
    return book;
  }
  if (active_book_count_ >= books_.size()) {
    ASTRA_TRACE("book_manager active book capacity exhausted locate=%u",
                stock_locate);
    return nullptr;
  }

  const uint16_t effective_channel =
      locate_registered_[stock_locate] ? channel_by_locate_[stock_locate]
                                       : channel_id;
  const size_t order_capacity =
      order_capacity_by_locate_[stock_locate] != 0
          ? order_capacity_by_locate_[stock_locate]
          : astra::capacity::kDefaultOrderCapacity;
  books_[active_book_count_] =
      std::make_unique<OrderBook>(static_cast<uint32_t>(stock_locate),
                                  effective_channel, order_capacity);
  book = books_[active_book_count_].get();
  ASTRA_TRACE("book_manager created locate=%u book=%p channel=%u capacity=%zu active_before=%u",
              stock_locate, static_cast<void *>(book), effective_channel,
              order_capacity, active_book_count_);
  ++active_book_count_;
  book_by_locate_[stock_locate] = book;
  return book;
}

void BookManager::registerStockLocate(uint16_t stock_locate,
                                      uint16_t channel_id) noexcept {
  ASTRA_TRACE("book_manager register locate=%u channel=%u", stock_locate,
              channel_id);
  if (stock_locate == 0 || stock_locate >= kMaxStockLocate) return;
  locate_registered_[stock_locate] = true;
  channel_by_locate_[stock_locate] = channel_id;
  if (OrderBook *book = book_by_locate_[stock_locate])
    book->setChannelId(channel_id);
}

void BookManager::setSymbolTier(uint16_t stock_locate,
                                astra::capacity::SymbolTier tier) noexcept {
  setSymbolOrderCapacity(stock_locate, astra::capacity::orderCapacity(tier));
}

void BookManager::setSymbolOrderCapacity(uint16_t stock_locate,
                                         size_t order_capacity) noexcept {
  ASTRA_TRACE("book_manager set capacity locate=%u capacity=%zu", stock_locate,
              order_capacity);
  if (stock_locate == 0 || stock_locate >= kMaxStockLocate ||
      order_capacity == 0) {
    return;
  }

  // Capacity affects books created after Stock Directory. Existing books keep
  // their arena; resizing an active book would invalidate order indices.
  if (book_by_locate_[stock_locate] == nullptr)
    order_capacity_by_locate_[stock_locate] = order_capacity;
}

void BookManager::prepareBook(uint16_t stock_locate, uint16_t channel_id,
                              bool touch_pages) noexcept {
  OrderBook *book = getOrCreate(stock_locate, channel_id);
  if (book != nullptr && touch_pages) {
    book->warmStorage();
  }
}

void BookManager::addOrder(uint16_t stock_locate, uint64_t order_id,
                           uint64_t price, uint32_t qty, OrderSide side,
                           uint16_t channel_id) noexcept {
  ASTRA_TRACE("book_manager add locate=%u order=%llu price=%llu qty=%u side=%c channel=%u",
              stock_locate, static_cast<unsigned long long>(order_id),
              static_cast<unsigned long long>(price), qty,
              side == OrderSide::Buy ? 'B' : 'S', channel_id);
  OrderBook *book = getOrCreate(stock_locate, channel_id);
  if (book) book->addOrder(order_id, price, qty, side);
  ASTRA_TRACE("book_manager add done locate=%u order=%llu book=%p",
              stock_locate, static_cast<unsigned long long>(order_id),
              static_cast<void *>(book));
}

void BookManager::cancelShares(uint16_t stock_locate, uint64_t order_id,
                               uint32_t canceled_qty) noexcept {
  ASTRA_TRACE("book_manager cancel locate=%u order=%llu qty=%u", stock_locate,
              static_cast<unsigned long long>(order_id), canceled_qty);
  OrderBook *book = find(stock_locate);
  if (book) book->cancelShares(order_id, canceled_qty);
  ASTRA_TRACE("book_manager cancel done locate=%u order=%llu book=%p",
              stock_locate, static_cast<unsigned long long>(order_id),
              static_cast<void *>(book));
}

void BookManager::deleteOrder(uint16_t stock_locate, uint64_t order_id) noexcept {
  ASTRA_TRACE("book_manager delete locate=%u order=%llu", stock_locate,
              static_cast<unsigned long long>(order_id));
  OrderBook *book = find(stock_locate);
  if (book) book->deleteOrder(order_id);
  ASTRA_TRACE("book_manager delete done locate=%u order=%llu book=%p",
              stock_locate, static_cast<unsigned long long>(order_id),
              static_cast<void *>(book));
}

void BookManager::trade(uint16_t stock_locate, uint64_t order_id,
                        uint32_t executed_qty) noexcept {
  ASTRA_TRACE("book_manager trade locate=%u order=%llu qty=%u", stock_locate,
              static_cast<unsigned long long>(order_id), executed_qty);
  OrderBook *book = find(stock_locate);
  if (book) book->trade(order_id, executed_qty);
  ASTRA_TRACE("book_manager trade done locate=%u order=%llu book=%p",
              stock_locate, static_cast<unsigned long long>(order_id),
              static_cast<void *>(book));
}

void BookManager::replaceOrder(uint16_t stock_locate, uint64_t old_id,
                               uint64_t new_id, uint64_t new_price,
                               uint32_t new_qty) noexcept {
  ASTRA_TRACE("book_manager replace locate=%u old=%llu new=%llu price=%llu qty=%u",
              stock_locate, static_cast<unsigned long long>(old_id),
              static_cast<unsigned long long>(new_id),
              static_cast<unsigned long long>(new_price), new_qty);
  OrderBook *book = find(stock_locate);
  if (book) book->replaceOrder(old_id, new_id, new_price, new_qty);
  ASTRA_TRACE("book_manager replace done locate=%u old=%llu new=%llu book=%p",
              stock_locate, static_cast<unsigned long long>(old_id),
              static_cast<unsigned long long>(new_id),
              static_cast<void *>(book));
}

void BookManager::reverseExecution(uint16_t stock_locate, uint64_t order_id,
                                   uint64_t price, uint32_t qty,
                                   OrderSide side) noexcept {
  OrderBook *book = find(stock_locate);
  if (book) book->reverseExecution(order_id, price, qty, side);
}

const OrderBook *BookManager::getOrderBook(uint16_t stock_locate) const noexcept {
  return find(stock_locate);
}
