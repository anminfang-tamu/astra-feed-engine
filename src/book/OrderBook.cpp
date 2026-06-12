#include "astra/book/OrderBook.hpp"

#include <cstddef>
#include <limits>

namespace {

constexpr uint32_t kPriceCenter =
    static_cast<uint32_t>(OrderBook::kMaxPriceLevels / 2);
constexpr uint64_t kInvalidOrderId = 0;

constexpr uint64_t nextPowerOfTwo(uint64_t v) noexcept {
  if (v <= 2)
    return 2;
  --v;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v |= v >> 32;
  return ++v;
}

uint32_t orderIndexCapacity(size_t order_capacity) noexcept {
  const uint64_t capacity = nextPowerOfTwo(order_capacity * 4);
  if (capacity > std::numeric_limits<uint32_t>::max())
    return std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(capacity);
}

bool isValidSide(char side) noexcept { return side == 'B' || side == 'S'; }

void touchPages(void *data, size_t bytes) noexcept {
  if (data == nullptr || bytes == 0) {
    return;
  }

  static constexpr size_t kPageSize = 4096;
  volatile char *p = static_cast<volatile char *>(data);
  for (size_t offset = 0; offset < bytes; offset += kPageSize) {
    p[offset] = p[offset];
  }
  p[bytes - 1] = p[bytes - 1];
}

void resetLevel(PriceLevel &level) noexcept {
  level.head_idx = OrderBook::kInvalidIdx;
  level.tail_idx = OrderBook::kInvalidIdx;
  level.total_qty = 0;
  level.num_orders = 0;
}

void resetOrder(Order &order) noexcept {
  order.prev_idx = OrderBook::kInvalidIdx;
  order.next_idx = OrderBook::kInvalidIdx;
  order.qty = 0;
  order.level_idx = OrderBook::kInvalidIdx;
  order.price = 0;
  order.order_id = kInvalidOrderId;
  order.side = 'B';
}

uint64_t indexToPrice(uint64_t reference_price, uint64_t tick_size,
                      uint32_t level_idx) noexcept {
  if (level_idx >= kPriceCenter) {
    return reference_price +
           (static_cast<uint64_t>(level_idx - kPriceCenter) * tick_size);
  }
  const uint64_t ticks_below =
      static_cast<uint64_t>(kPriceCenter - level_idx) * tick_size;
  return reference_price >= ticks_below ? reference_price - ticks_below : 0;
}

bool appendOrder(FixedMmapArray<Order> &orders, PriceLevel &level,
                 uint32_t order_idx) noexcept {
  if (order_idx >= orders.size())
    return false;
  Order &order = orders[order_idx];
  order.prev_idx = level.tail_idx;
  order.next_idx = OrderBook::kInvalidIdx;
  if (level.num_orders == 0) {
    level.head_idx = order_idx;
  } else {
    if (level.tail_idx >= orders.size()) {
      order.prev_idx = OrderBook::kInvalidIdx;
      return false;
    }
    orders[level.tail_idx].next_idx = order_idx;
  }
  level.tail_idx = order_idx;
  return true;
}

bool unlinkOrder(FixedMmapArray<Order> &orders, PriceLevel &level,
                 uint32_t order_idx) noexcept {
  if (order_idx >= orders.size() || level.num_orders == 0)
    return false;

  Order &order = orders[order_idx];
  const bool has_prev = order.prev_idx != OrderBook::kInvalidIdx;
  const bool has_next = order.next_idx != OrderBook::kInvalidIdx;

  if (has_prev && order.prev_idx >= orders.size())
    return false;
  if (has_next && order.next_idx >= orders.size())
    return false;
  if (!has_prev && level.head_idx != order_idx)
    return false;
  if (!has_next && level.tail_idx != order_idx) {
    if (level.num_orders == 2 && level.head_idx == order_idx &&
        level.tail_idx < orders.size()) {
      Order &remaining = orders[level.tail_idx];
      remaining.prev_idx = OrderBook::kInvalidIdx;
      remaining.next_idx = OrderBook::kInvalidIdx;
      level.head_idx = level.tail_idx;
      order.prev_idx = OrderBook::kInvalidIdx;
      order.next_idx = OrderBook::kInvalidIdx;
      return true;
    }
    return false;
  }
  if (!has_prev && !has_next && level.num_orders > 1) {
    if (level.num_orders == 2 && level.tail_idx < orders.size() &&
        level.tail_idx != order_idx) {
      Order &remaining = orders[level.tail_idx];
      remaining.prev_idx = OrderBook::kInvalidIdx;
      remaining.next_idx = OrderBook::kInvalidIdx;
      level.head_idx = level.tail_idx;
      order.prev_idx = OrderBook::kInvalidIdx;
      order.next_idx = OrderBook::kInvalidIdx;
      return true;
    }
    return false;
  }

  if (has_prev)
    orders[order.prev_idx].next_idx = order.next_idx;
  else
    level.head_idx = order.next_idx;

  if (has_next)
    orders[order.next_idx].prev_idx = order.prev_idx;
  else
    level.tail_idx = order.prev_idx;

  order.prev_idx = OrderBook::kInvalidIdx;
  order.next_idx = OrderBook::kInvalidIdx;
  return true;
}

} // namespace

OrderBook::OrderBook(uint32_t symbol_id, size_t order_capacity)
    : symbol_id_(symbol_id), order_capacity_(order_capacity),
      reference_price_(0), tick_size_(1), order_pool_(order_capacity),
      free_list_(order_capacity), free_list_size_(0), next_order_idx_(0),
      bid_levels_(kMaxPriceLevels), ask_levels_(kMaxPriceLevels),
      order_index_(orderIndexCapacity(order_capacity)),
      stock_locate_(static_cast<uint16_t>(symbol_id)) {
  touchPages(order_pool_.data(), order_pool_.size() * sizeof(Order));
  touchPages(free_list_.data(), free_list_.size() * sizeof(uint32_t));
  touchPages(bid_levels_.data(), bid_levels_.size() * sizeof(PriceLevel));
  touchPages(ask_levels_.data(), ask_levels_.size() * sizeof(PriceLevel));
  order_index_.warmPages();
}

uint32_t OrderBook::priceToIndex(uint64_t price) const noexcept {
  if (tick_size_ == 0)
    return kInvalidIdx;
  if (price >= reference_price_) {
    const uint64_t diff = price - reference_price_;
    if (diff % tick_size_ != 0)
      return kInvalidIdx;
    const uint64_t ticks = diff / tick_size_;
    if (ticks > std::numeric_limits<uint32_t>::max() ||
        ticks + kPriceCenter >= kMaxPriceLevels)
      return kInvalidIdx;
    return kPriceCenter + static_cast<uint32_t>(ticks);
  }
  const uint64_t diff = reference_price_ - price;
  if (diff % tick_size_ != 0)
    return kInvalidIdx;
  const uint64_t ticks = diff / tick_size_;
  if (ticks > kPriceCenter)
    return kInvalidIdx;
  return kPriceCenter - static_cast<uint32_t>(ticks);
}

uint32_t OrderBook::allocateOrder() noexcept {
  uint32_t idx = kInvalidIdx;
  if (free_list_size_ != 0) {
    idx = free_list_[--free_list_size_];
  } else {
    if (next_order_idx_ >= order_pool_.size()) {
      ++allocation_failures_;
      return kInvalidIdx;
    }
    idx = static_cast<uint32_t>(next_order_idx_++);
  }
  if (idx >= order_pool_.size())
    return kInvalidIdx;
  resetOrder(order_pool_[idx]);
  return idx;
}

void OrderBook::freeOrder(uint32_t order_idx) noexcept {
  if (order_idx >= order_pool_.size())
    return;
  Order &order = order_pool_[order_idx];
  if (order.order_id != kInvalidOrderId)
    order_index_.erase(order.order_id);
  resetOrder(order);
  if (free_list_size_ < free_list_.size())
    free_list_[free_list_size_++] = order_idx;
}

bool OrderBook::removeFromLevel(uint32_t order_idx) noexcept {

  if (order_idx >= order_pool_.size())
    return false;

  Order &order = order_pool_[order_idx];
  if (order.order_id == kInvalidOrderId || order.level_idx >= kMaxPriceLevels) {
    local_invalid_ = true;
    return false;
  }

  std::vector<PriceLevel> &levels =
      order.side == 'B' ? bid_levels_ : ask_levels_;
  PriceBitmap &bitmap = order.side == 'B' ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &level = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < order.qty ||
      level.head_idx >= order_pool_.size() ||
      level.tail_idx >= order_pool_.size()) {
    local_invalid_ = true;
    return false;
  }

  if (!unlinkOrder(order_pool_, level, order_idx)) {
    local_invalid_ = true;
    return false;
  }
  level.total_qty -= order.qty;
  --level.num_orders;
  if (level.num_orders == 0) {
    resetLevel(level);
    bitmap.reset(order.level_idx);
  } else if (level.head_idx >= order_pool_.size() ||
             level.tail_idx >= order_pool_.size()) {
    local_invalid_ = true;

    return false;
  }
  return true;
}

void OrderBook::addOrder(uint64_t order_id, uint64_t price, uint32_t qty,
                         char side) noexcept {
  if (qty == 0 || !isValidSide(side) || order_id == kInvalidOrderId ||
      order_index_.find(order_id) != kInvalidIdx) {
    return;
  }

  if (bid_bitmap_.empty() && ask_bitmap_.empty())
    reference_price_ = price;

  const uint32_t level_idx = priceToIndex(price);
  if (level_idx == kInvalidIdx) {
    return;
  }

  const uint32_t order_idx = allocateOrder();
  if (order_idx == kInvalidIdx)
    return;

  std::vector<PriceLevel> &levels = side == 'B' ? bid_levels_ : ask_levels_;
  PriceBitmap &bitmap = side == 'B' ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &level = levels[level_idx];
  const bool was_empty = level.num_orders == 0;

  Order &order = order_pool_[order_idx];
  order.qty = qty;
  order.level_idx = level_idx;
  order.price = price;
  order.order_id = order_id;
  order.side = side;

  if (!order_index_.insert(order.order_id, order_idx)) {
    order.order_id = kInvalidOrderId;
    freeOrder(order_idx);
    return;
  }

  if (!appendOrder(order_pool_, level, order_idx)) {
    local_invalid_ = true;
    order_index_.erase(order.order_id);
    order.order_id = kInvalidOrderId;
    freeOrder(order_idx);
    return;
  }
  level.total_qty += qty;
  ++level.num_orders;
  if (was_empty)
    bitmap.set(level_idx);
}

void OrderBook::cancelShares(uint64_t order_id,
                             uint32_t canceled_qty) noexcept {
  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != order_id) {
    return;
  }
  if (canceled_qty >= order.qty) {
    deleteOrder(order_id);
    return;
  }

  std::vector<PriceLevel> &levels =
      order.side == 'B' ? bid_levels_ : ask_levels_;
  PriceLevel &level = levels[order.level_idx];

  level.total_qty -= canceled_qty;
  order.qty -= canceled_qty;
}

void OrderBook::deleteOrder(uint64_t order_id) noexcept {
  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != order_id) {
    return;
  }
  std::vector<PriceLevel> &levels =
      order.side == 'B' ? bid_levels_ : ask_levels_;
  PriceLevel &level = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < order.qty) {
    return;
  }

  if (!removeFromLevel(order_idx))
    return;

  freeOrder(order_idx);
}

bool OrderBook::trade(uint64_t order_id, uint32_t executed_qty) noexcept {
  if (executed_qty == 0)
    return false;
  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    return false;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != order_id) {
    return false;
  }
  if (executed_qty > order.qty) {
    return false;
  }

  std::vector<PriceLevel> &levels =
      order.side == 'B' ? bid_levels_ : ask_levels_;
  PriceLevel &level = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < executed_qty) {
    return false;
  }

  if (executed_qty == order.qty) {
    if (!removeFromLevel(order_idx))
      return false;
    freeOrder(order_idx);
  } else {
    level.total_qty -= executed_qty;
    order.qty -= executed_qty;
  }
  return true;
}

void OrderBook::replaceOrder(uint64_t old_id, uint64_t new_id,
                             uint64_t new_price, uint32_t new_qty) noexcept {
  if (new_qty == 0 || new_id == kInvalidOrderId) {
    return;
  }

  const uint32_t order_idx = order_index_.find(old_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != old_id) {
    return;
  }
  const char side = order.side;

  if (!removeFromLevel(order_idx)) {
    return;
  }

  order_index_.erase(old_id);
  order.order_id = kInvalidOrderId;

  const uint32_t new_level_idx = priceToIndex(new_price);
  if (new_level_idx == kInvalidIdx) {
    resetOrder(order);
    if (free_list_size_ < free_list_.size())
      free_list_[free_list_size_++] = order_idx;
    return;
  }

  std::vector<PriceLevel> &levels = side == 'B' ? bid_levels_ : ask_levels_;
  PriceBitmap &bitmap = side == 'B' ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &level = levels[new_level_idx];
  const bool was_empty = level.num_orders == 0;

  resetOrder(order);
  order.qty = new_qty;
  order.level_idx = new_level_idx;
  order.price = new_price;
  order.order_id = new_id;
  order.side = side;

  if (!order_index_.insert(new_id, order_idx)) {
    resetOrder(order);
    if (free_list_size_ < free_list_.size())
      free_list_[free_list_size_++] = order_idx;
    return;
  }

  if (!appendOrder(order_pool_, level, order_idx)) {
    local_invalid_ = true;
    order_index_.erase(order.order_id);
    order.order_id = kInvalidOrderId;
    freeOrder(order_idx);
    return;
  }
  level.total_qty += new_qty;
  ++level.num_orders;
  if (was_empty)
    bitmap.set(new_level_idx);
}

void OrderBook::reverseExecution(uint64_t order_id, uint64_t price,
                                 uint32_t qty, char side) noexcept {
  if (qty == 0)
    return;

  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx != kInvalidIdx && order_idx < order_pool_.size()) {
    // Order still exists — add qty back to existing order and level
    Order &order = order_pool_[order_idx];
    std::vector<PriceLevel> &levels =
        order.side == 'B' ? bid_levels_ : ask_levels_;
    PriceLevel &level = levels[order.level_idx];
    order.qty += qty;
    level.total_qty += qty;
  } else {
    // Order was fully consumed — re-add it
    addOrder(order_id, price, qty, side);
  }
}

TopOfBook OrderBook::getTopOfBook() const noexcept {
  TopOfBook top{};
  top.symbol_id = symbol_id_;

  const uint32_t bid_idx =
      bid_bitmap_.findHighestAtOrBelow(PriceBitmap::kNumBits - 1);
  if (bid_idx != PriceBitmap::kInvalid) {
    top.bid_price = indexToPrice(reference_price_, tick_size_, bid_idx);
    top.bid_qty = bid_levels_[bid_idx].total_qty;
  }

  const uint32_t ask_idx = ask_bitmap_.findLowestAtOrAbove(0);
  if (ask_idx != PriceBitmap::kInvalid) {
    top.ask_price = indexToPrice(reference_price_, tick_size_, ask_idx);
    top.ask_qty = ask_levels_[ask_idx].total_qty;
  }

  return top;
}

BookUpdate OrderBook::getBookUpdate() const noexcept {
  BookUpdate update{};
  update.symbol_id = symbol_id_;

  uint32_t bid_idx =
      bid_bitmap_.findHighestAtOrBelow(PriceBitmap::kNumBits - 1);
  while (bid_idx != PriceBitmap::kInvalid &&
         update.bids_depth < update.bids.size()) {
    const PriceLevel &level = bid_levels_[bid_idx];
    update.bids[update.bids_depth++] = {
        indexToPrice(reference_price_, tick_size_, bid_idx), level.total_qty,
        level.num_orders};
    if (bid_idx == 0)
      break;
    bid_idx = bid_bitmap_.findHighestAtOrBelow(bid_idx - 1);
  }

  uint32_t ask_idx = ask_bitmap_.findLowestAtOrAbove(0);
  while (ask_idx != PriceBitmap::kInvalid &&
         update.asks_depth < update.asks.size()) {
    const PriceLevel &level = ask_levels_[ask_idx];
    update.asks[update.asks_depth++] = {
        indexToPrice(reference_price_, tick_size_, ask_idx), level.total_qty,
        level.num_orders};
    if (ask_idx == PriceBitmap::kNumBits - 1)
      break;
    ask_idx = ask_bitmap_.findLowestAtOrAbove(ask_idx + 1);
  }

  return update;
}
