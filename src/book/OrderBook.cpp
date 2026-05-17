#include "astra/book/OrderBook.hpp"

#include "astra/protocol/OrderSide.hpp"

#include <limits>

namespace {

constexpr uint32_t kPriceCenter =
    static_cast<uint32_t>(OrderBook::kMaxPriceLevels / 2);
constexpr uint32_t kOrderIdMapCapacity =
    static_cast<uint32_t>(OrderBook::kOrderPoolSize * 2);
constexpr uint64_t kInvalidOrderId = 0;

bool isValidSide(OrderSide side) noexcept {
  return side == OrderSide::Buy || side == OrderSide::Sell;
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
  order.side = OrderSide::Buy;
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

void appendOrder(std::vector<Order> &orders, PriceLevel &level,
                 uint32_t order_idx) noexcept {
  Order &order = orders[order_idx];
  order.prev_idx = level.tail_idx;
  order.next_idx = OrderBook::kInvalidIdx;

  if (level.num_orders == 0) {
    level.head_idx = order_idx;
  } else {
    orders[level.tail_idx].next_idx = order_idx;
  }

  level.tail_idx = order_idx;
}

void unlinkOrder(std::vector<Order> &orders, PriceLevel &level,
                 uint32_t order_idx) noexcept {
  Order &order = orders[order_idx];

  if (order.prev_idx != OrderBook::kInvalidIdx) {
    orders[order.prev_idx].next_idx = order.next_idx;
  } else {
    level.head_idx = order.next_idx;
  }

  if (order.next_idx != OrderBook::kInvalidIdx) {
    orders[order.next_idx].prev_idx = order.prev_idx;
  } else {
    level.tail_idx = order.prev_idx;
  }

  order.prev_idx = OrderBook::kInvalidIdx;
  order.next_idx = OrderBook::kInvalidIdx;
}

} // namespace

OrderBook::OrderBook(uint32_t symbol_id)
    : symbol_id_(symbol_id), reference_price_(0), tick_size_(1),
      order_id_map_(kOrderIdMapCapacity) {
}

bool OrderBook::ensureLevelStorage() noexcept {
  if (bid_levels_.size() == kMaxPriceLevels &&
      ask_levels_.size() == kMaxPriceLevels) {
    return true;
  }

  try {
    bid_levels_.resize(kMaxPriceLevels);
    ask_levels_.resize(kMaxPriceLevels);

    for (PriceLevel &level : bid_levels_) {
      resetLevel(level);
    }
    for (PriceLevel &level : ask_levels_) {
      resetLevel(level);
    }
  } catch (...) {
    std::vector<PriceLevel>().swap(bid_levels_);
    std::vector<PriceLevel>().swap(ask_levels_);
    return false;
  }

  return true;
}

uint32_t OrderBook::priceToIndex(uint64_t price) const noexcept {
  if (tick_size_ == 0) {
    return kInvalidIdx;
  }

  if (price >= reference_price_) {
    const uint64_t diff = price - reference_price_;
    if (diff % tick_size_ != 0) {
      return kInvalidIdx;
    }

    const uint64_t ticks = diff / tick_size_;
    if (ticks > std::numeric_limits<uint32_t>::max() ||
        ticks + kPriceCenter >= kMaxPriceLevels) {
      return kInvalidIdx;
    }
    return kPriceCenter + static_cast<uint32_t>(ticks);
  }

  const uint64_t diff = reference_price_ - price;
  if (diff % tick_size_ != 0) {
    return kInvalidIdx;
  }

  const uint64_t ticks = diff / tick_size_;
  if (ticks > kPriceCenter) {
    return kInvalidIdx;
  }
  return kPriceCenter - static_cast<uint32_t>(ticks);
}

uint32_t OrderBook::allocateOrder() noexcept {
  if (!free_list_.empty()) {
    const uint32_t order_idx = free_list_.back();
    free_list_.pop_back();
    resetOrder(order_pool_[order_idx]);
    return order_idx;
  }

  if (order_pool_.size() >= kOrderPoolSize) {
    return kInvalidIdx;
  }

  const uint32_t order_idx = static_cast<uint32_t>(order_pool_.size());
  try {
    order_pool_.push_back(Order{});
  } catch (...) {
    return kInvalidIdx;
  }
  resetOrder(order_pool_.back());
  return order_idx;
}

void OrderBook::freeOrder(uint32_t order_idx) noexcept {
  if (order_idx >= order_pool_.size()) {
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != kInvalidOrderId) {
    order_id_map_.erase(order.order_id);
  }
  resetOrder(order);
  try {
    free_list_.push_back(order_idx);
  } catch (...) {
  }
}

void OrderBook::addOrder(const MarketDataMessageView &msg) noexcept {
  if (msg.qty() == 0 || !isValidSide(msg.side()) ||
      msg.orderId() == kInvalidOrderId ||
      order_id_map_.contains(msg.orderId())) {
    return;
  }

  if (bid_bitmap_.empty() && ask_bitmap_.empty()) {
    reference_price_ = msg.price();
  }

  const uint32_t level_idx = priceToIndex(msg.price());
  if (level_idx == kInvalidIdx) {
    return;
  }

  if (!ensureLevelStorage()) {
    return;
  }

  const uint32_t order_idx = allocateOrder();
  if (order_idx == kInvalidIdx) {
    return;
  }

  std::vector<PriceLevel> &levels =
      msg.side() == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap &bitmap =
      msg.side() == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;

  PriceLevel &level = levels[level_idx];
  const bool was_empty = level.num_orders == 0;

  Order &order = order_pool_[order_idx];
  order.qty = msg.qty();
  order.level_idx = level_idx;
  order.price = msg.price();
  order.order_id = msg.orderId();
  order.side = msg.side();

  bool inserted = false;
  try {
    inserted = order_id_map_.emplace(order.order_id, order_idx).second;
  } catch (...) {
    inserted = false;
  }

  if (!inserted) {
    order.order_id = kInvalidOrderId;
    freeOrder(order_idx);
    return;
  }

  appendOrder(order_pool_, level, order_idx);
  level.total_qty += order.qty;
  ++level.num_orders;
  if (was_empty) {
    bitmap.set(level_idx);
  }
}

void OrderBook::modifyOrder(const MarketDataMessageView &msg) noexcept {
  const auto order_it = order_id_map_.find(msg.orderId());
  if (order_it == order_id_map_.end() ||
      order_it->second >= order_pool_.size()) {
    return;
  }
  const uint32_t order_idx = order_it->second;

  if (msg.qty() == 0) {
    deleteOrder(msg);
    return;
  }

  if (!isValidSide(msg.side())) {
    return;
  }

  const uint32_t new_level_idx = priceToIndex(msg.price());
  if (new_level_idx == kInvalidIdx) {
    return;
  }

  Order &order = order_pool_[order_idx];
  std::vector<PriceLevel> &old_levels =
      order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap &old_bitmap =
      order.side == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &old_level = old_levels[order.level_idx];

  if (old_level.num_orders == 0 || old_level.total_qty < order.qty) {
    return;
  }

  if (order.price == msg.price() && order.side == msg.side()) {
    old_level.total_qty = old_level.total_qty - order.qty + msg.qty();
    order.qty = msg.qty();
    return;
  }

  unlinkOrder(order_pool_, old_level, order_idx);
  old_level.total_qty -= order.qty;
  --old_level.num_orders;
  if (old_level.num_orders == 0) {
    resetLevel(old_level);
    old_bitmap.reset(order.level_idx);
  }

  std::vector<PriceLevel> &new_levels =
      msg.side() == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap &new_bitmap =
      msg.side() == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &new_level = new_levels[new_level_idx];
  const bool new_level_was_empty = new_level.num_orders == 0;

  order.qty = msg.qty();
  order.level_idx = new_level_idx;
  order.price = msg.price();
  order.side = msg.side();

  appendOrder(order_pool_, new_level, order_idx);
  new_level.total_qty += order.qty;
  ++new_level.num_orders;
  if (new_level_was_empty) {
    new_bitmap.set(new_level_idx);
  }
}

void OrderBook::deleteOrder(const MarketDataMessageView &msg) noexcept {
  const auto order_it = order_id_map_.find(msg.orderId());
  if (order_it == order_id_map_.end() ||
      order_it->second >= order_pool_.size()) {
    return;
  }
  const uint32_t order_idx = order_it->second;

  Order &order = order_pool_[order_idx];
  std::vector<PriceLevel> &levels =
      order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap &bitmap =
      order.side == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &level = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < order.qty) {
    return;
  }

  unlinkOrder(order_pool_, level, order_idx);
  level.total_qty -= order.qty;
  --level.num_orders;
  if (level.num_orders == 0) {
    resetLevel(level);
    bitmap.reset(order.level_idx);
  }

  freeOrder(order_idx);
}

void OrderBook::trade(const MarketDataMessageView &msg) noexcept {
  if (msg.qty() == 0) {
    return;
  }

  const auto order_it = order_id_map_.find(msg.orderId());
  if (order_it == order_id_map_.end() ||
      order_it->second >= order_pool_.size()) {
    return;
  }
  const uint32_t order_idx = order_it->second;

  Order &order = order_pool_[order_idx];
  if (msg.qty() > order.qty) {
    return;
  }

  std::vector<PriceLevel> &levels =
      order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap &bitmap =
      order.side == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel &level = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < msg.qty()) {
    return;
  }

  level.total_qty -= msg.qty();
  if (msg.qty() == order.qty) {
    unlinkOrder(order_pool_, level, order_idx);
    --level.num_orders;
    if (level.num_orders == 0) {
      resetLevel(level);
      bitmap.reset(order.level_idx);
    }
    freeOrder(order_idx);
  } else {
    order.qty -= msg.qty();
  }
}

TopOfBook OrderBook::getTopOfBook() const noexcept {
  TopOfBook top{};
  top.symbol_id = symbol_id_;

  const uint32_t bid_idx =
      bid_bitmap_.findHighestAtOrBelow(PriceBitmap::kNumBits - 1);
  if (bid_idx != PriceBitmap::kInvalid) {
    const PriceLevel &bid = bid_levels_[bid_idx];
    top.bid_price = indexToPrice(reference_price_, tick_size_, bid_idx);
    top.bid_qty = bid.total_qty;
  }

  const uint32_t ask_idx = ask_bitmap_.findLowestAtOrAbove(0);
  if (ask_idx != PriceBitmap::kInvalid) {
    const PriceLevel &ask = ask_levels_[ask_idx];
    top.ask_price = indexToPrice(reference_price_, tick_size_, ask_idx);
    top.ask_qty = ask.total_qty;
  }

  return top;
}

BookUpdate OrderBook::getBookUpdate() const noexcept {
  BookUpdate update{};
  update.symbol_id = symbol_id_;

  uint32_t bid_idx = bid_bitmap_.findHighestAtOrBelow(PriceBitmap::kNumBits - 1);
  while (bid_idx != PriceBitmap::kInvalid &&
         update.bids_depth < update.bids.size()) {
    const PriceLevel &level = bid_levels_[bid_idx];
    update.bids[update.bids_depth] = {
        indexToPrice(reference_price_, tick_size_, bid_idx),
        level.total_qty,
        level.num_orders,
    };
    ++update.bids_depth;

    if (bid_idx == 0) {
      break;
    }
    bid_idx = bid_bitmap_.findHighestAtOrBelow(bid_idx - 1);
  }

  uint32_t ask_idx = ask_bitmap_.findLowestAtOrAbove(0);
  while (ask_idx != PriceBitmap::kInvalid &&
         update.asks_depth < update.asks.size()) {
    const PriceLevel &level = ask_levels_[ask_idx];
    update.asks[update.asks_depth] = {
        indexToPrice(reference_price_, tick_size_, ask_idx),
        level.total_qty,
        level.num_orders,
    };
    ++update.asks_depth;

    if (ask_idx == PriceBitmap::kNumBits - 1) {
      break;
    }
    ask_idx = ask_bitmap_.findLowestAtOrAbove(ask_idx + 1);
  }

  return update;
}
