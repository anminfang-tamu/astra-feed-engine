#include "astra/book/OrderBook.hpp"
#include "astra/protocol/OrderSide.hpp"
#include "astra/utils/DebugTrace.hpp"

#include <limits>

namespace {

constexpr uint32_t kPriceCenter = static_cast<uint32_t>(OrderBook::kMaxPriceLevels / 2);
constexpr uint64_t kInvalidOrderId = 0;

constexpr uint64_t nextPowerOfTwo(uint64_t v) noexcept {
  if (v <= 2) return 2;
  --v;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v |= v >> 32;
  return ++v;
}

uint32_t orderIndexCapacity(size_t order_capacity) noexcept {
  const uint64_t capacity = nextPowerOfTwo(order_capacity * 4);
  if (capacity > std::numeric_limits<uint32_t>::max())
    return std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(capacity);
}

bool isValidSide(OrderSide side) noexcept {
  return side == OrderSide::Buy || side == OrderSide::Sell;
}

void resetLevel(PriceLevel &level) noexcept {
  level.head_idx  = OrderBook::kInvalidIdx;
  level.tail_idx  = OrderBook::kInvalidIdx;
  level.total_qty = 0;
  level.num_orders = 0;
}

void resetOrder(Order &order) noexcept {
  order.prev_idx  = OrderBook::kInvalidIdx;
  order.next_idx  = OrderBook::kInvalidIdx;
  order.qty       = 0;
  order.level_idx = OrderBook::kInvalidIdx;
  order.price     = 0;
  order.order_id  = kInvalidOrderId;
  order.side      = OrderSide::Buy;
}

uint64_t indexToPrice(uint64_t reference_price, uint64_t tick_size,
                      uint32_t level_idx) noexcept {
  if (level_idx >= kPriceCenter) {
    return reference_price + (static_cast<uint64_t>(level_idx - kPriceCenter) * tick_size);
  }
  const uint64_t ticks_below = static_cast<uint64_t>(kPriceCenter - level_idx) * tick_size;
  return reference_price >= ticks_below ? reference_price - ticks_below : 0;
}

bool appendOrder(FixedMmapArray<Order> &orders, PriceLevel &level,
                 uint32_t order_idx) noexcept {
  ASTRA_TRACE("book append enter order_idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u",
              order_idx, level.head_idx, level.tail_idx, level.total_qty,
              level.num_orders);
  if (order_idx >= orders.size()) return false;
  Order &order = orders[order_idx];
  order.prev_idx = level.tail_idx;
  order.next_idx = OrderBook::kInvalidIdx;
  if (level.num_orders == 0) {
    level.head_idx = order_idx;
  } else {
    if (level.tail_idx >= orders.size()) {
      ASTRA_TRACE("book append invalid tail order_idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u capacity=%zu",
                  order_idx, level.head_idx, level.tail_idx, level.total_qty,
                  level.num_orders, orders.size());
      order.prev_idx = OrderBook::kInvalidIdx;
      return false;
    }
    orders[level.tail_idx].next_idx = order_idx;
  }
  level.tail_idx = order_idx;
  ASTRA_TRACE("book append exit order_idx=%u prev=%u next=%u level_head=%u level_tail=%u",
              order_idx, order.prev_idx, order.next_idx, level.head_idx,
              level.tail_idx);
  return true;
}

bool unlinkOrder(FixedMmapArray<Order> &orders, PriceLevel &level,
                 uint32_t order_idx) noexcept {
  ASTRA_TRACE("book unlink enter order_idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u",
              order_idx, level.head_idx, level.tail_idx, level.total_qty,
              level.num_orders);
  if (order_idx >= orders.size() || level.num_orders == 0) return false;

  Order &order = orders[order_idx];
  const bool has_prev = order.prev_idx != OrderBook::kInvalidIdx;
  const bool has_next = order.next_idx != OrderBook::kInvalidIdx;

  if (has_prev && order.prev_idx >= orders.size()) return false;
  if (has_next && order.next_idx >= orders.size()) return false;
  if (!has_prev && level.head_idx != order_idx) return false;
  if (!has_next && level.tail_idx != order_idx) {
    if (level.num_orders == 2 && level.head_idx == order_idx &&
        level.tail_idx < orders.size()) {
      Order &remaining = orders[level.tail_idx];
      remaining.prev_idx = OrderBook::kInvalidIdx;
      remaining.next_idx = OrderBook::kInvalidIdx;
      level.head_idx = level.tail_idx;
      order.prev_idx = OrderBook::kInvalidIdx;
      order.next_idx = OrderBook::kInvalidIdx;
      ASTRA_TRACE("book unlink repaired missing next order_idx=%u remaining=%u",
                  order_idx, level.tail_idx);
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
      ASTRA_TRACE("book unlink repaired isolated head order_idx=%u remaining=%u",
                  order_idx, level.tail_idx);
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
  ASTRA_TRACE("book unlink exit order_idx=%u level_head=%u level_tail=%u",
              order_idx, level.head_idx, level.tail_idx);
  return true;
}

} // namespace

OrderBook::OrderBook(uint32_t symbol_id, uint16_t channel_id,
                     size_t order_capacity)
    : symbol_id_(symbol_id), order_capacity_(order_capacity),
      reference_price_(0), tick_size_(1),
      order_pool_(order_capacity), free_list_(order_capacity),
      free_list_size_(0), next_order_idx_(0),
      bid_levels_(kMaxPriceLevels), ask_levels_(kMaxPriceLevels),
      order_index_(orderIndexCapacity(order_capacity)), channel_id_(channel_id),
      stock_locate_(static_cast<uint16_t>(symbol_id)) {
  ASTRA_TRACE("book ctor this=%p symbol=%u channel=%u order_capacity=%zu",
              static_cast<void *>(this), symbol_id, channel_id,
              order_capacity);
}

uint32_t OrderBook::priceToIndex(uint64_t price) const noexcept {
  if (tick_size_ == 0) return kInvalidIdx;
  if (price >= reference_price_) {
    const uint64_t diff = price - reference_price_;
    if (diff % tick_size_ != 0) return kInvalidIdx;
    const uint64_t ticks = diff / tick_size_;
    if (ticks > std::numeric_limits<uint32_t>::max() || ticks + kPriceCenter >= kMaxPriceLevels)
      return kInvalidIdx;
    return kPriceCenter + static_cast<uint32_t>(ticks);
  }
  const uint64_t diff = reference_price_ - price;
  if (diff % tick_size_ != 0) return kInvalidIdx;
  const uint64_t ticks = diff / tick_size_;
  if (ticks > kPriceCenter) return kInvalidIdx;
  return kPriceCenter - static_cast<uint32_t>(ticks);
}

uint32_t OrderBook::allocateOrder() noexcept {
  ASTRA_TRACE("book allocate enter symbol=%u next=%zu free=%zu capacity=%zu",
              symbol_id_, next_order_idx_, free_list_size_, order_pool_.size());
  uint32_t idx = kInvalidIdx;
  if (free_list_size_ != 0) {
    idx = free_list_[--free_list_size_];
  } else {
    if (next_order_idx_ >= order_pool_.size()) {
      ++allocation_failures_;
      ASTRA_TRACE("book allocate failed symbol=%u failures=%llu", symbol_id_,
                  static_cast<unsigned long long>(allocation_failures_));
      return kInvalidIdx;
    }
    idx = static_cast<uint32_t>(next_order_idx_++);
  }
  if (idx >= order_pool_.size()) return kInvalidIdx;
  resetOrder(order_pool_[idx]);
  ASTRA_TRACE("book allocate exit symbol=%u idx=%u next=%zu free=%zu",
              symbol_id_, idx, next_order_idx_, free_list_size_);
  return idx;
}

void OrderBook::freeOrder(uint32_t order_idx) noexcept {
  ASTRA_TRACE("book free enter symbol=%u idx=%u free=%zu", symbol_id_,
              order_idx, free_list_size_);
  if (order_idx >= order_pool_.size()) return;
  Order &order = order_pool_[order_idx];
  if (order.order_id != kInvalidOrderId)
    order_index_.erase(order.order_id);
  resetOrder(order);
  if (free_list_size_ < free_list_.size())
    free_list_[free_list_size_++] = order_idx;
  ASTRA_TRACE("book free exit symbol=%u idx=%u free=%zu", symbol_id_,
              order_idx, free_list_size_);
}

bool OrderBook::removeFromLevel(uint32_t order_idx) noexcept {
  ASTRA_TRACE("book removeFromLevel enter symbol=%u idx=%u", symbol_id_,
              order_idx);
  if (order_idx >= order_pool_.size()) return false;

  Order &order = order_pool_[order_idx];
  if (order.order_id == kInvalidOrderId || order.level_idx >= kMaxPriceLevels) {
    local_invalid_ = true;
    ASTRA_TRACE("book removeFromLevel invalid order symbol=%u idx=%u order_id=%llu level_idx=%u",
                symbol_id_, order_idx,
                static_cast<unsigned long long>(order.order_id),
                order.level_idx);
    return false;
  }

  FixedMmapArray<PriceLevel> &levels = order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap             &bitmap = order.side == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel              &level  = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < order.qty ||
      level.head_idx >= order_pool_.size() ||
      level.tail_idx >= order_pool_.size()) {
    local_invalid_ = true;
    ASTRA_TRACE("book removeFromLevel invalid level symbol=%u idx=%u order=%llu level_idx=%u head=%u tail=%u level_qty=%u level_orders=%u order_qty=%u capacity=%zu",
                symbol_id_, order_idx,
                static_cast<unsigned long long>(order.order_id),
                order.level_idx, level.head_idx, level.tail_idx,
                level.total_qty, level.num_orders, order.qty,
                order_pool_.size());
    return false;
  }

  if (!unlinkOrder(order_pool_, level, order_idx)) {
    local_invalid_ = true;
    ASTRA_TRACE("book removeFromLevel unlink failed symbol=%u idx=%u order=%llu level_idx=%u head=%u tail=%u level_qty=%u level_orders=%u",
                symbol_id_, order_idx,
                static_cast<unsigned long long>(order.order_id),
                order.level_idx, level.head_idx, level.tail_idx,
                level.total_qty, level.num_orders);
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
    ASTRA_TRACE("book removeFromLevel post invalid symbol=%u idx=%u level_idx=%u head=%u tail=%u level_qty=%u level_orders=%u",
                symbol_id_, order_idx, order.level_idx, level.head_idx,
                level.tail_idx, level.total_qty, level.num_orders);
    return false;
  }
  ASTRA_TRACE("book removeFromLevel exit symbol=%u idx=%u level_idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u",
              symbol_id_, order_idx, order.level_idx, level.head_idx,
              level.tail_idx, level.total_qty, level.num_orders);
  return true;
}

void OrderBook::addOrder(uint64_t order_id, uint64_t price, uint32_t qty,
                         OrderSide side) noexcept {
  ASTRA_TRACE("book add enter symbol=%u order=%llu price=%llu qty=%u side=%c",
              symbol_id_, static_cast<unsigned long long>(order_id),
              static_cast<unsigned long long>(price), qty,
              side == OrderSide::Buy ? 'B' : 'S');
  if (qty == 0 || !isValidSide(side) || order_id == kInvalidOrderId ||
      order_index_.find(order_id) != kInvalidIdx) {
    ASTRA_TRACE("book add ignored symbol=%u order=%llu", symbol_id_,
                static_cast<unsigned long long>(order_id));
    return;
  }

  if (bid_bitmap_.empty() && ask_bitmap_.empty())
    reference_price_ = price;

  const uint32_t level_idx = priceToIndex(price);
  if (level_idx == kInvalidIdx) {
    ASTRA_TRACE("book add invalid price symbol=%u order=%llu price=%llu",
                symbol_id_, static_cast<unsigned long long>(order_id),
                static_cast<unsigned long long>(price));
    return;
  }

  const uint32_t order_idx = allocateOrder();
  if (order_idx == kInvalidIdx) return;

  FixedMmapArray<PriceLevel> &levels = side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap             &bitmap = side == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel              &level  = levels[level_idx];
  const bool was_empty = level.num_orders == 0;

  Order &order    = order_pool_[order_idx];
  order.qty       = qty;
  order.level_idx = level_idx;
  order.price     = price;
  order.order_id  = order_id;
  order.side      = side;

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
  if (was_empty) bitmap.set(level_idx);
  ASTRA_TRACE("book add exit symbol=%u order=%llu idx=%u level_idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u",
              symbol_id_, static_cast<unsigned long long>(order_id),
              order_idx, level_idx, level.head_idx, level.tail_idx,
              level.total_qty, level.num_orders);
}

void OrderBook::cancelShares(uint64_t order_id, uint32_t canceled_qty) noexcept {
  ASTRA_TRACE("book cancel enter symbol=%u order=%llu qty=%u", symbol_id_,
              static_cast<unsigned long long>(order_id), canceled_qty);
  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    ASTRA_TRACE("book cancel missing symbol=%u order=%llu idx=%u", symbol_id_,
                static_cast<unsigned long long>(order_id), order_idx);
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != order_id) {
    ASTRA_TRACE("book cancel stale index symbol=%u requested=%llu idx=%u actual=%llu",
                symbol_id_, static_cast<unsigned long long>(order_id),
                order_idx, static_cast<unsigned long long>(order.order_id));
    return;
  }
  if (canceled_qty >= order.qty) {
    ASTRA_TRACE("book cancel routes delete symbol=%u order=%llu canceled=%u order_qty=%u",
                symbol_id_, static_cast<unsigned long long>(order_id),
                canceled_qty, order.qty);
    deleteOrder(order_id);
    return;
  }

  FixedMmapArray<PriceLevel> &levels = order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceLevel              &level  = levels[order.level_idx];

  level.total_qty -= canceled_qty;
  order.qty       -= canceled_qty;
  ASTRA_TRACE("book cancel exit symbol=%u order=%llu idx=%u remaining=%u level_idx=%u level_qty=%u",
              symbol_id_, static_cast<unsigned long long>(order_id),
              order_idx, order.qty, order.level_idx, level.total_qty);
}

void OrderBook::deleteOrder(uint64_t order_id) noexcept {
  ASTRA_TRACE("book delete enter symbol=%u order=%llu", symbol_id_,
              static_cast<unsigned long long>(order_id));
  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    ASTRA_TRACE("book delete missing symbol=%u order=%llu idx=%u", symbol_id_,
                static_cast<unsigned long long>(order_id), order_idx);
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != order_id) {
    ASTRA_TRACE("book delete stale index symbol=%u requested=%llu idx=%u actual=%llu",
                symbol_id_, static_cast<unsigned long long>(order_id),
                order_idx, static_cast<unsigned long long>(order.order_id));
    return;
  }
  FixedMmapArray<PriceLevel> &levels = order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceLevel              &level  = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < order.qty) {
    ASTRA_TRACE("book delete invalid level symbol=%u order=%llu idx=%u level_idx=%u head=%u tail=%u level_qty=%u level_orders=%u order_qty=%u",
                symbol_id_, static_cast<unsigned long long>(order_id),
                order_idx, order.level_idx, level.head_idx, level.tail_idx,
                level.total_qty, level.num_orders, order.qty);
    return;
  }

  if (!removeFromLevel(order_idx)) return;

  freeOrder(order_idx);
  ASTRA_TRACE("book delete exit symbol=%u order=%llu idx=%u", symbol_id_,
              static_cast<unsigned long long>(order_id), order_idx);
}

void OrderBook::trade(uint64_t order_id, uint32_t executed_qty) noexcept {
  ASTRA_TRACE("book trade enter symbol=%u order=%llu qty=%u", symbol_id_,
              static_cast<unsigned long long>(order_id), executed_qty);
  if (executed_qty == 0) return;
  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    ASTRA_TRACE("book trade missing symbol=%u order=%llu idx=%u", symbol_id_,
                static_cast<unsigned long long>(order_id), order_idx);
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != order_id) {
    ASTRA_TRACE("book trade stale index symbol=%u requested=%llu idx=%u actual=%llu",
                symbol_id_, static_cast<unsigned long long>(order_id),
                order_idx, static_cast<unsigned long long>(order.order_id));
    return;
  }
  if (executed_qty > order.qty) {
    ASTRA_TRACE("book trade overfill symbol=%u order=%llu exec=%u order_qty=%u",
                symbol_id_, static_cast<unsigned long long>(order_id),
                executed_qty, order.qty);
    return;
  }

  FixedMmapArray<PriceLevel> &levels = order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceLevel              &level  = levels[order.level_idx];

  if (level.num_orders == 0 || level.total_qty < executed_qty) {
    ASTRA_TRACE("book trade invalid level symbol=%u order=%llu idx=%u level_idx=%u head=%u tail=%u level_qty=%u level_orders=%u exec=%u",
                symbol_id_, static_cast<unsigned long long>(order_id),
                order_idx, order.level_idx, level.head_idx, level.tail_idx,
                level.total_qty, level.num_orders, executed_qty);
    return;
  }

  if (executed_qty == order.qty) {
    if (!removeFromLevel(order_idx)) return;
    freeOrder(order_idx);
  } else {
    level.total_qty -= executed_qty;
    order.qty -= executed_qty;
  }
  ASTRA_TRACE("book trade exit symbol=%u order=%llu idx=%u level_idx=%u level_qty=%u order_qty=%u",
              symbol_id_, static_cast<unsigned long long>(order_id),
              order_idx, order.level_idx, level.total_qty, order.qty);
}

void OrderBook::replaceOrder(uint64_t old_id, uint64_t new_id,
                             uint64_t new_price, uint32_t new_qty) noexcept {
  ASTRA_TRACE("book replace enter symbol=%u old=%llu new=%llu price=%llu qty=%u",
              symbol_id_, static_cast<unsigned long long>(old_id),
              static_cast<unsigned long long>(new_id),
              static_cast<unsigned long long>(new_price), new_qty);
  if (new_qty == 0 || new_id == kInvalidOrderId) {
    ASTRA_TRACE("book replace ignored invalid new symbol=%u old=%llu new=%llu qty=%u",
                symbol_id_, static_cast<unsigned long long>(old_id),
                static_cast<unsigned long long>(new_id), new_qty);
    return;
  }

  const uint32_t order_idx = order_index_.find(old_id);
  if (order_idx == kInvalidIdx || order_idx >= order_pool_.size()) {
    ASTRA_TRACE("book replace missing old symbol=%u old=%llu idx=%u",
                symbol_id_, static_cast<unsigned long long>(old_id),
                order_idx);
    return;
  }

  Order &order = order_pool_[order_idx];
  if (order.order_id != old_id) {
    ASTRA_TRACE("book replace stale index symbol=%u old=%llu idx=%u actual=%llu",
                symbol_id_, static_cast<unsigned long long>(old_id),
                order_idx, static_cast<unsigned long long>(order.order_id));
    return;
  }
  const OrderSide side = order.side;
  ASTRA_TRACE("book replace found old symbol=%u old=%llu idx=%u side=%c old_level=%u old_qty=%u old_price=%llu",
              symbol_id_, static_cast<unsigned long long>(old_id),
              order_idx, side == OrderSide::Buy ? 'B' : 'S',
              order.level_idx, order.qty,
              static_cast<unsigned long long>(order.price));

  if (!removeFromLevel(order_idx)) {
    ASTRA_TRACE("book replace removeFromLevel failed symbol=%u old=%llu idx=%u",
                symbol_id_, static_cast<unsigned long long>(old_id),
                order_idx);
    return;
  }
  ASTRA_TRACE("book replace removed old symbol=%u old=%llu idx=%u",
              symbol_id_, static_cast<unsigned long long>(old_id),
              order_idx);
  order_index_.erase(old_id);
  order.order_id = kInvalidOrderId;
  ASTRA_TRACE("book replace erased old index symbol=%u old=%llu idx=%u",
              symbol_id_, static_cast<unsigned long long>(old_id),
              order_idx);

  const uint32_t new_level_idx = priceToIndex(new_price);
  if (new_level_idx == kInvalidIdx) {
    ASTRA_TRACE("book replace invalid new price symbol=%u old=%llu new=%llu price=%llu",
                symbol_id_, static_cast<unsigned long long>(old_id),
                static_cast<unsigned long long>(new_id),
                static_cast<unsigned long long>(new_price));
    resetOrder(order);
    if (free_list_size_ < free_list_.size())
      free_list_[free_list_size_++] = order_idx;
    return;
  }

  FixedMmapArray<PriceLevel> &levels = side == OrderSide::Buy ? bid_levels_ : ask_levels_;
  PriceBitmap             &bitmap = side == OrderSide::Buy ? bid_bitmap_ : ask_bitmap_;
  PriceLevel              &level  = levels[new_level_idx];
  const bool was_empty = level.num_orders == 0;
  ASTRA_TRACE("book replace target level symbol=%u new=%llu idx=%u new_level=%u head=%u tail=%u level_qty=%u level_orders=%u was_empty=%d capacity=%zu",
              symbol_id_, static_cast<unsigned long long>(new_id),
              order_idx, new_level_idx, level.head_idx, level.tail_idx,
              level.total_qty, level.num_orders, was_empty ? 1 : 0,
              order_pool_.size());

  resetOrder(order);
  order.qty       = new_qty;
  order.level_idx = new_level_idx;
  order.price     = new_price;
  order.order_id  = new_id;
  order.side      = side;

  if (!order_index_.insert(new_id, order_idx)) {
    ASTRA_TRACE("book replace insert new failed symbol=%u new=%llu idx=%u",
                symbol_id_, static_cast<unsigned long long>(new_id),
                order_idx);
    resetOrder(order);
    if (free_list_size_ < free_list_.size())
      free_list_[free_list_size_++] = order_idx;
    return;
  }

  ASTRA_TRACE("book replace before append symbol=%u new=%llu idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u order_prev=%u order_next=%u",
              symbol_id_, static_cast<unsigned long long>(new_id),
              order_idx, level.head_idx, level.tail_idx, level.total_qty,
              level.num_orders, order.prev_idx, order.next_idx);
  if (!appendOrder(order_pool_, level, order_idx)) {
    local_invalid_ = true;
    order_index_.erase(order.order_id);
    order.order_id = kInvalidOrderId;
    freeOrder(order_idx);
    return;
  }
  ASTRA_TRACE("book replace after append symbol=%u new=%llu idx=%u level_head=%u level_tail=%u level_qty=%u level_orders=%u",
              symbol_id_, static_cast<unsigned long long>(new_id),
              order_idx, level.head_idx, level.tail_idx, level.total_qty,
              level.num_orders);
  level.total_qty += new_qty;
  ++level.num_orders;
  if (was_empty) bitmap.set(new_level_idx);
  ASTRA_TRACE("book replace exit symbol=%u old=%llu new=%llu idx=%u level_idx=%u level_qty=%u level_orders=%u",
              symbol_id_, static_cast<unsigned long long>(old_id),
              static_cast<unsigned long long>(new_id), order_idx,
              new_level_idx, level.total_qty, level.num_orders);
}

void OrderBook::reverseExecution(uint64_t order_id, uint64_t price,
                                 uint32_t qty, OrderSide side) noexcept {
  if (qty == 0) return;

  const uint32_t order_idx = order_index_.find(order_id);
  if (order_idx != kInvalidIdx && order_idx < order_pool_.size()) {
    // Order still exists — add qty back to existing order and level
    Order &order = order_pool_[order_idx];
    FixedMmapArray<PriceLevel> &levels = order.side == OrderSide::Buy ? bid_levels_ : ask_levels_;
    PriceLevel              &level  = levels[order.level_idx];
    order.qty       += qty;
    level.total_qty += qty;
  } else {
    // Order was fully consumed — re-add it
    addOrder(order_id, price, qty, side);
  }
}

TopOfBook OrderBook::getTopOfBook() const noexcept {
  TopOfBook top{};
  top.symbol_id = symbol_id_;

  const uint32_t bid_idx = bid_bitmap_.findHighestAtOrBelow(PriceBitmap::kNumBits - 1);
  if (bid_idx != PriceBitmap::kInvalid) {
    top.bid_price = indexToPrice(reference_price_, tick_size_, bid_idx);
    top.bid_qty   = bid_levels_[bid_idx].total_qty;
  }

  const uint32_t ask_idx = ask_bitmap_.findLowestAtOrAbove(0);
  if (ask_idx != PriceBitmap::kInvalid) {
    top.ask_price = indexToPrice(reference_price_, tick_size_, ask_idx);
    top.ask_qty   = ask_levels_[ask_idx].total_qty;
  }

  return top;
}

BookUpdate OrderBook::getBookUpdate() const noexcept {
  BookUpdate update{};
  update.symbol_id = symbol_id_;

  uint32_t bid_idx = bid_bitmap_.findHighestAtOrBelow(PriceBitmap::kNumBits - 1);
  while (bid_idx != PriceBitmap::kInvalid && update.bids_depth < update.bids.size()) {
    const PriceLevel &level = bid_levels_[bid_idx];
    update.bids[update.bids_depth++] = {
        indexToPrice(reference_price_, tick_size_, bid_idx),
        level.total_qty, level.num_orders};
    if (bid_idx == 0) break;
    bid_idx = bid_bitmap_.findHighestAtOrBelow(bid_idx - 1);
  }

  uint32_t ask_idx = ask_bitmap_.findLowestAtOrAbove(0);
  while (ask_idx != PriceBitmap::kInvalid && update.asks_depth < update.asks.size()) {
    const PriceLevel &level = ask_levels_[ask_idx];
    update.asks[update.asks_depth++] = {
        indexToPrice(reference_price_, tick_size_, ask_idx),
        level.total_qty, level.num_orders};
    if (ask_idx == PriceBitmap::kNumBits - 1) break;
    ask_idx = ask_bitmap_.findLowestAtOrAbove(ask_idx + 1);
  }

  return update;
}
