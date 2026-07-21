#include "astra/book/OrderBook.hpp"

#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace {

constexpr uint64_t kInvalidOrderId = 0;

bool isValidSide(char side) noexcept { return side == 'B' || side == 'S'; }

PriceLevelIndex::Side priceSide(char side) noexcept {
  return side == 'B' ? PriceLevelIndex::Side::Bid
                     : PriceLevelIndex::Side::Ask;
}

void resetOrder(Order &order) noexcept {
  order.order_id = kInvalidOrderId;
  order.price = 0;
  order.prev_idx = OrderBook::kInvalidIdx;
  order.next_idx = OrderBook::kInvalidIdx;
  order.qty = 0;
  order.setLevelAndSide(Order::kInvalidLevel, 'B');
}

bool appendOrder(OrderArena &orders, PooledPriceLevel &level,
                 uint32_t order_idx) noexcept {
  Order *order = orders.at(order_idx);
  if (order == nullptr) {
    return false;
  }

  order->prev_idx = PooledPriceLevel::kInvalidOrderIndex;
  order->next_idx = PooledPriceLevel::kInvalidOrderIndex;

  if (level.num_orders == 0) {
    if (level.head_idx != PooledPriceLevel::kInvalidOrderIndex ||
        level.tail_idx != PooledPriceLevel::kInvalidOrderIndex ||
        level.total_qty != 0) {
      return false;
    }
    level.head_idx = order_idx;
    level.tail_idx = order_idx;
    return true;
  }

  Order *tail = orders.at(level.tail_idx);
  if (orders.at(level.head_idx) == nullptr || tail == nullptr ||
      tail->order_id == kInvalidOrderId ||
      tail->next_idx != PooledPriceLevel::kInvalidOrderIndex) {
    return false;
  }

  order->prev_idx = level.tail_idx;
  tail->next_idx = order_idx;
  level.tail_idx = order_idx;
  return true;
}

bool unlinkOrder(OrderArena &orders, PooledPriceLevel &level,
                 uint32_t order_idx) noexcept {
  Order *order = orders.at(order_idx);
  if (order == nullptr || level.num_orders == 0 ||
      orders.at(level.head_idx) == nullptr ||
      orders.at(level.tail_idx) == nullptr) {
    return false;
  }

  const uint32_t prev = order->prev_idx;
  const uint32_t next = order->next_idx;
  const bool has_prev = prev != PooledPriceLevel::kInvalidOrderIndex;
  const bool has_next = next != PooledPriceLevel::kInvalidOrderIndex;

  if (level.num_orders == 1) {
    if (level.head_idx != order_idx || level.tail_idx != order_idx ||
        has_prev || has_next) {
      return false;
    }
    level.head_idx = PooledPriceLevel::kInvalidOrderIndex;
    level.tail_idx = PooledPriceLevel::kInvalidOrderIndex;
    return true;
  }

  if ((!has_prev && level.head_idx != order_idx) ||
      (!has_next && level.tail_idx != order_idx) ||
      (has_prev && orders.at(prev) == nullptr) ||
      (has_next && orders.at(next) == nullptr) || (!has_prev && !has_next)) {
    return false;
  }
  Order *previous_order = has_prev ? orders.at(prev) : nullptr;
  Order *next_order = has_next ? orders.at(next) : nullptr;
  if (has_prev && (previous_order == nullptr ||
                   previous_order->order_id == kInvalidOrderId ||
                   previous_order->next_idx != order_idx)) {
    return false;
  }
  if (has_next &&
      (next_order == nullptr || next_order->order_id == kInvalidOrderId ||
       next_order->prev_idx != order_idx)) {
    return false;
  }

  if (has_prev) {
    previous_order->next_idx = next;
  } else {
    level.head_idx = next;
  }
  if (has_next) {
    next_order->prev_idx = prev;
  } else {
    level.tail_idx = prev;
  }
  order->prev_idx = PooledPriceLevel::kInvalidOrderIndex;
  order->next_idx = PooledPriceLevel::kInvalidOrderIndex;
  return true;
}

} // namespace

size_t OrderBook::indexCapacityForOrders(size_t order_capacity) {
  if (order_capacity == 0 ||
      order_capacity > static_cast<size_t>(OrderArena::kInvalidIndex) ||
      order_capacity > std::numeric_limits<size_t>::max() / 4) {
    throw std::invalid_argument(
        "per-book order capacity is outside the supported index domain");
  }
  const size_t requested_slots = order_capacity * 4;
  if (requested_slots >
      std::bit_floor(std::numeric_limits<size_t>::max())) {
    throw std::length_error("per-book order-reference map is too large");
  }
  const size_t slots = std::bit_ceil(requested_slots);
  return slots;
}

OrderBook::OrderBook(uint32_t symbol_id, size_t order_capacity,
                     PriceLevelArena &price_level_arena)
    : symbol_id_(symbol_id),
      order_refs_(indexCapacityForOrders(order_capacity)),
      order_arena_(order_capacity), price_level_arena_(price_level_arena),
      price_level_index_(price_level_arena),
      stock_locate_(static_cast<uint16_t>(symbol_id)) {}

void OrderBook::recordMutationFailure() noexcept {
  ++mutation_failures_;
  local_invalid_ = true;
}

bool OrderBook::resolveOrderRef(uint64_t order_id,
                                uint32_t &order_idx) noexcept {
  order_idx = order_refs_.find(order_id);
  if (order_idx == LocalOrderRefMap::kInvalidIndex) {
    ++missing_order_refs_;
    recordMutationFailure();
    return false;
  }
  if (!hasOrderAt(order_idx, order_id)) {
    if (!order_refs_.erase(order_id)) {
      ++index_erase_failures_;
    }
    ++stale_order_refs_;
    ++missing_order_refs_;
    recordMutationFailure();
    order_idx = kInvalidIdx;
    return false;
  }
  return true;
}

bool OrderBook::eraseOrderRef(uint64_t order_id) noexcept {
  if (order_refs_.erase(order_id)) {
    return true;
  }
  ++index_erase_failures_;
  recordMutationFailure();
  return false;
}

void OrderBook::addOrder(uint64_t order_id, uint64_t price, uint32_t qty,
                         char side) noexcept {
  if (qty == 0 || !isValidSide(side) || order_id == kInvalidOrderId) {
    ++invalid_adds_;
    return;
  }

  const uint32_t existing = order_refs_.find(order_id);
  if (existing != LocalOrderRefMap::kInvalidIndex) {
    if (!hasOrderAt(existing, order_id)) {
      if (!order_refs_.erase(order_id)) {
        ++index_erase_failures_;
      }
      ++stale_order_refs_;
    } else {
      ++duplicate_order_refs_;
    }
    recordMutationFailure();
    return;
  }

  if (order_refs_.size() >= order_refs_.maxEntries()) {
    ++index_insert_failures_;
    recordMutationFailure();
    return;
  }

  const uint32_t order_idx = addOrderIndexed(order_id, price, qty, side);
  if (order_idx == kInvalidIdx) {
    return;
  }

  const LocalOrderRefMap::InsertResult inserted =
      order_refs_.insert(order_id, order_idx);
  if (inserted == LocalOrderRefMap::InsertResult::Inserted) {
    return;
  }

  ++index_insert_failures_;
  if (inserted == LocalOrderRefMap::InsertResult::Duplicate) {
    ++duplicate_order_refs_;
  }
  (void)deleteOrderIndexed(order_idx, order_id);
  recordMutationFailure();
}

void OrderBook::cancelShares(uint64_t order_id,
                             uint32_t canceled_qty) noexcept {
  uint32_t order_idx = kInvalidIdx;
  if (!resolveOrderRef(order_id, order_idx)) {
    return;
  }
  const MutationResult result =
      cancelSharesIndexed(order_idx, order_id, canceled_qty);
  if (result == MutationResult::Removed) {
    (void)eraseOrderRef(order_id);
  } else if (result == MutationResult::Ignored) {
    recordMutationFailure();
  }
}

void OrderBook::deleteOrder(uint64_t order_id) noexcept {
  uint32_t order_idx = kInvalidIdx;
  if (!resolveOrderRef(order_id, order_idx)) {
    return;
  }
  if (deleteOrderIndexed(order_idx, order_id)) {
    (void)eraseOrderRef(order_id);
  } else {
    recordMutationFailure();
  }
}

bool OrderBook::trade(uint64_t order_id, uint32_t executed_qty) noexcept {
  uint32_t order_idx = kInvalidIdx;
  if (!resolveOrderRef(order_id, order_idx)) {
    return false;
  }
  const MutationResult result =
      tradeIndexed(order_idx, order_id, executed_qty);
  if (result == MutationResult::Removed) {
    (void)eraseOrderRef(order_id);
  } else if (result == MutationResult::Ignored) {
    recordMutationFailure();
  }
  return result != MutationResult::Ignored;
}

void OrderBook::replaceOrder(uint64_t old_id, uint64_t new_id,
                             uint64_t new_price, uint32_t new_qty) noexcept {
  uint32_t order_idx = kInvalidIdx;
  if (!resolveOrderRef(old_id, order_idx)) {
    return;
  }

  if (new_id != old_id) {
    const uint32_t existing = order_refs_.find(new_id);
    if (existing != LocalOrderRefMap::kInvalidIndex) {
      if (!hasOrderAt(existing, new_id)) {
        if (!order_refs_.erase(new_id)) {
          ++index_erase_failures_;
        }
        ++stale_order_refs_;
      } else {
        ++duplicate_order_refs_;
      }
      recordMutationFailure();
      return;
    }
  }

  const MutationResult result = replaceOrderIndexed(
      order_idx, old_id, new_id, new_price, new_qty);
  if (result == MutationResult::Ignored) {
    recordMutationFailure();
    return;
  }
  if (result == MutationResult::Removed) {
    (void)eraseOrderRef(old_id);
    return;
  }

  const LocalOrderRefMap::ReplaceResult replaced =
      order_refs_.replaceKey(old_id, new_id);
  if (replaced == LocalOrderRefMap::ReplaceResult::Replaced) {
    return;
  }

  ++index_replace_failures_;
  (void)deleteOrderIndexed(order_idx, new_id);
  if (!order_refs_.erase(old_id)) {
    ++index_erase_failures_;
  }
  recordMutationFailure();
}

void OrderBook::reverseExecution(uint64_t order_id, uint64_t price,
                                 uint32_t qty, char side) noexcept {
  if (qty == 0) {
    return;
  }

  const uint32_t order_idx = order_refs_.find(order_id);
  if (order_idx != LocalOrderRefMap::kInvalidIndex) {
    const Order *order = orderAt(order_idx);
    if (order == nullptr || order->order_id != order_id) {
      if (!order_refs_.erase(order_id)) {
        ++index_erase_failures_;
      }
      ++stale_order_refs_;
      ++missing_order_refs_;
      recordMutationFailure();
      return;
    }
    if (!isValidSide(side) || order->price != price ||
        order->side() != side) {
      recordMutationFailure();
      return;
    }
    if (restoreSharesIndexed(order_idx, order_id, qty) ==
        MutationResult::Updated) {
      return;
    }
    recordMutationFailure();
    return;
  }
  addOrder(order_id, price, qty, side);
}

uint32_t OrderBook::allocateOrder() noexcept {
  const uint32_t idx = order_arena_.allocate();
  Order *order = order_arena_.at(idx);
  if (idx == OrderArena::kInvalidIndex || order == nullptr) {
    local_invalid_ = true;
    return kInvalidIdx;
  }
  resetOrder(*order);
  ++live_order_count_;
  return idx;
}

void OrderBook::freeOrder(uint32_t order_idx) noexcept {
  if (!order_arena_.release(order_idx) || live_order_count_ == 0) {
    local_invalid_ = true;
    return;
  }
  --live_order_count_;
}

bool OrderBook::hasOrderAt(uint32_t order_idx, uint64_t order_id) const
    noexcept {
  const Order *order = order_arena_.at(order_idx);
  return order != nullptr && order->order_id == order_id;
}

const Order *OrderBook::orderAt(uint32_t order_idx) const noexcept {
  return order_arena_.at(order_idx);
}

bool OrderBook::removeFromLevel(uint32_t order_idx) noexcept {
  Order *order = order_arena_.at(order_idx);
  if (order == nullptr) {
    return false;
  }

  const PriceLevelHandle handle{order->levelIndex()};
  if (order->order_id == kInvalidOrderId || !handle.valid() ||
      order->price > std::numeric_limits<uint32_t>::max()) {
    local_invalid_ = true;
    return false;
  }

  PooledPriceLevel *level = price_level_arena_.level(handle);
  if (level == nullptr || level->num_orders == 0 ||
      level->total_qty < order->qty ||
      order_arena_.at(level->head_idx) == nullptr ||
      order_arena_.at(level->tail_idx) == nullptr) {
    local_invalid_ = true;
    return false;
  }

  if (!unlinkOrder(order_arena_, *level, order_idx)) {
    local_invalid_ = true;
    return false;
  }

  level->total_qty -= order->qty;
  --level->num_orders;
  if (level->num_orders == 0) {
    if (level->total_qty != 0 ||
        level->head_idx != PooledPriceLevel::kInvalidOrderIndex ||
        level->tail_idx != PooledPriceLevel::kInvalidOrderIndex ||
        price_level_index_.eraseEmpty(
            static_cast<uint32_t>(order->price), priceSide(order->side()),
            handle) != PriceLevelIndex::EraseStatus::Erased) {
      local_invalid_ = true;
      return false;
    }
  } else if (order_arena_.at(level->head_idx) == nullptr ||
             order_arena_.at(level->tail_idx) == nullptr) {
    local_invalid_ = true;
    return false;
  }
  return true;
}

uint32_t OrderBook::addOrderIndexed(uint64_t order_id, uint64_t price,
                                    uint32_t qty, char side) noexcept {
  if (qty == 0 || !isValidSide(side) || order_id == kInvalidOrderId ||
      price > std::numeric_limits<uint32_t>::max()) {
    if (price > std::numeric_limits<uint32_t>::max()) {
      ++price_rejections_;
      local_invalid_ = true;
    }
    return kInvalidIdx;
  }

  const uint32_t raw_price = static_cast<uint32_t>(price);
  const PriceLevelIndex::Side index_side = priceSide(side);
  const PriceLevelIndex::EnsureResult ensured =
      price_level_index_.ensure(raw_price, index_side);
  if (!ensured.ok()) {
    ++price_rejections_;
    local_invalid_ = true;
    return kInvalidIdx;
  }

  PooledPriceLevel *level = price_level_arena_.level(ensured.handle);
  if (level == nullptr ||
      level->total_qty > std::numeric_limits<uint64_t>::max() - qty) {
    (void)price_level_index_.rollbackEnsure(raw_price, index_side, ensured);
    local_invalid_ = true;
    return kInvalidIdx;
  }

  const uint32_t order_idx = allocateOrder();
  if (order_idx == kInvalidIdx) {
    if (ensured.created() &&
        !price_level_index_.rollbackEnsure(raw_price, index_side, ensured)) {
      local_invalid_ = true;
    }
    return kInvalidIdx;
  }

  Order *order = order_arena_.at(order_idx);
  if (order == nullptr) {
    local_invalid_ = true;
    return kInvalidIdx;
  }
  order->qty = qty;
  order->price = price;
  order->order_id = order_id;
  order->setLevelAndSide(ensured.handle.value, side);

  if (!appendOrder(order_arena_, *level, order_idx)) {
    local_invalid_ = true;
    freeOrder(order_idx);
    (void)price_level_index_.rollbackEnsure(raw_price, index_side, ensured);
    return kInvalidIdx;
  }

  level->total_qty += qty;
  ++level->num_orders;
  return order_idx;
}

OrderBook::MutationResult
OrderBook::cancelSharesIndexed(uint32_t order_idx, uint64_t order_id,
                               uint32_t canceled_qty) noexcept {
  Order *order = order_arena_.at(order_idx);
  if (canceled_qty == 0 || order == nullptr) {
    return MutationResult::Ignored;
  }

  if (order->order_id != order_id || canceled_qty > order->qty) {
    return MutationResult::Ignored;
  }
  if (canceled_qty == order->qty) {
    return deleteOrderIndexed(order_idx, order_id)
               ? MutationResult::Removed
               : MutationResult::Ignored;
  }

  PooledPriceLevel *level =
      price_level_arena_.level(PriceLevelHandle{order->levelIndex()});
  if (level == nullptr || level->num_orders == 0 ||
      level->total_qty < canceled_qty) {
    local_invalid_ = true;
    return MutationResult::Ignored;
  }
  level->total_qty -= canceled_qty;
  order->qty -= canceled_qty;
  return MutationResult::Updated;
}

bool OrderBook::deleteOrderIndexed(uint32_t order_idx,
                                   uint64_t order_id) noexcept {
  const Order *order = order_arena_.at(order_idx);
  if (order == nullptr || order->order_id != order_id) {
    return false;
  }
  if (!removeFromLevel(order_idx)) {
    return false;
  }
  freeOrder(order_idx);
  return true;
}

OrderBook::MutationResult
OrderBook::tradeIndexed(uint32_t order_idx, uint64_t order_id,
                        uint32_t executed_qty) noexcept {
  Order *order = order_arena_.at(order_idx);
  if (executed_qty == 0 || order == nullptr) {
    return MutationResult::Ignored;
  }

  if (order->order_id != order_id || executed_qty > order->qty) {
    return MutationResult::Ignored;
  }
  if (executed_qty == order->qty) {
    return deleteOrderIndexed(order_idx, order_id)
               ? MutationResult::Removed
               : MutationResult::Ignored;
  }

  PooledPriceLevel *level =
      price_level_arena_.level(PriceLevelHandle{order->levelIndex()});
  if (level == nullptr || level->num_orders == 0 ||
      level->total_qty < executed_qty) {
    local_invalid_ = true;
    return MutationResult::Ignored;
  }
  level->total_qty -= executed_qty;
  order->qty -= executed_qty;
  return MutationResult::Updated;
}

OrderBook::MutationResult OrderBook::replaceOrderIndexed(
    uint32_t order_idx, uint64_t old_id, uint64_t new_id, uint64_t new_price,
    uint32_t new_qty) noexcept {
  if (new_qty == 0 || new_id == kInvalidOrderId ||
      new_price > std::numeric_limits<uint32_t>::max() ||
      order_arena_.at(order_idx) == nullptr) {
    if (new_price > std::numeric_limits<uint32_t>::max()) {
      ++price_rejections_;
      local_invalid_ = true;
    }
    return MutationResult::Ignored;
  }

  Order *order = order_arena_.at(order_idx);
  if (order == nullptr || order->order_id != old_id ||
      order->price > std::numeric_limits<uint32_t>::max()) {
    return MutationResult::Ignored;
  }

  const char side = order->side();
  const PriceLevelIndex::Side index_side = priceSide(side);
  const uint32_t raw_new_price = static_cast<uint32_t>(new_price);

  if (new_price == order->price) {
    const PriceLevelHandle existing_handle{order->levelIndex()};
    PooledPriceLevel *level = price_level_arena_.level(existing_handle);
    if (level == nullptr || level->num_orders == 0 ||
        level->total_qty < order->qty) {
      local_invalid_ = true;
      return MutationResult::Ignored;
    }
    const uint64_t remaining_qty = level->total_qty - order->qty;
    if (remaining_qty > std::numeric_limits<uint64_t>::max() - new_qty ||
        !unlinkOrder(order_arena_, *level, order_idx)) {
      local_invalid_ = true;
      return MutationResult::Ignored;
    }

    level->total_qty = remaining_qty;
    --level->num_orders;
    resetOrder(*order);
    order->qty = new_qty;
    order->price = new_price;
    order->order_id = new_id;
    order->setLevelAndSide(existing_handle.value, side);
    if (!appendOrder(order_arena_, *level, order_idx)) {
      local_invalid_ = true;
      if (level->num_orders == 0 && level->total_qty == 0) {
        (void)price_level_index_.eraseEmpty(raw_new_price, index_side,
                                            existing_handle);
      }
      freeOrder(order_idx);
      return MutationResult::Removed;
    }
    level->total_qty += new_qty;
    ++level->num_orders;
    return MutationResult::Updated;
  }

  const PriceLevelIndex::EnsureResult ensured =
      price_level_index_.ensure(raw_new_price, index_side);
  if (!ensured.ok()) {
    ++price_rejections_;
    local_invalid_ = true;
    return MutationResult::Ignored;
  }

  PooledPriceLevel *new_level = price_level_arena_.level(ensured.handle);
  if (new_level == nullptr ||
      new_level->total_qty > std::numeric_limits<uint64_t>::max() - new_qty) {
    (void)price_level_index_.rollbackEnsure(raw_new_price, index_side, ensured);
    local_invalid_ = true;
    return MutationResult::Ignored;
  }

  if (!removeFromLevel(order_idx)) {
    (void)price_level_index_.rollbackEnsure(raw_new_price, index_side, ensured);
    return MutationResult::Ignored;
  }

  resetOrder(*order);
  order->qty = new_qty;
  order->price = new_price;
  order->order_id = new_id;
  order->setLevelAndSide(ensured.handle.value, side);
  if (!appendOrder(order_arena_, *new_level, order_idx)) {
    local_invalid_ = true;
    freeOrder(order_idx);
    (void)price_level_index_.rollbackEnsure(raw_new_price, index_side, ensured);
    return MutationResult::Removed;
  }
  new_level->total_qty += new_qty;
  ++new_level->num_orders;
  return MutationResult::Updated;
}

OrderBook::MutationResult
OrderBook::restoreSharesIndexed(uint32_t order_idx, uint64_t order_id,
                                uint32_t qty) noexcept {
  Order *order = order_arena_.at(order_idx);
  if (qty == 0 || order == nullptr) {
    return MutationResult::Ignored;
  }

  PooledPriceLevel *level =
      price_level_arena_.level(PriceLevelHandle{order->levelIndex()});
  if (order->order_id != order_id || level == nullptr ||
      qty > std::numeric_limits<uint32_t>::max() - order->qty ||
      level->total_qty > std::numeric_limits<uint64_t>::max() - qty) {
    local_invalid_ = true;
    return MutationResult::Ignored;
  }
  order->qty += qty;
  level->total_qty += qty;
  return MutationResult::Updated;
}

TopOfBook OrderBook::getTopOfBook() const noexcept {
  TopOfBook top{};
  top.symbol_id = symbol_id_;

  const PriceLevelIndex::PricePoint bid =
      price_level_index_.best(PriceLevelIndex::Side::Bid);
  if (bid) {
    const PooledPriceLevel *level = price_level_arena_.level(bid.handle);
    if (level != nullptr) {
      top.has_bid = true;
      top.bid_price = bid.price;
      top.bid_qty = level->total_qty;
    }
  }

  const PriceLevelIndex::PricePoint ask =
      price_level_index_.best(PriceLevelIndex::Side::Ask);
  if (ask) {
    const PooledPriceLevel *level = price_level_arena_.level(ask.handle);
    if (level != nullptr) {
      top.has_ask = true;
      top.ask_price = ask.price;
      top.ask_qty = level->total_qty;
    }
  }
  return top;
}

BookUpdate OrderBook::getBookUpdate() const noexcept {
  BookUpdate update{};
  update.symbol_id = symbol_id_;

  for (PriceLevelIndex::PricePoint point =
           price_level_index_.best(PriceLevelIndex::Side::Bid);
       point && update.bids_depth < update.bids.size();
       point = price_level_index_.nextWorse(PriceLevelIndex::Side::Bid,
                                            point.price)) {
    const PooledPriceLevel *level = price_level_arena_.level(point.handle);
    if (level == nullptr) {
      break;
    }
    update.bids[update.bids_depth++] = {point.price, level->total_qty,
                                        level->num_orders};
  }

  for (PriceLevelIndex::PricePoint point =
           price_level_index_.best(PriceLevelIndex::Side::Ask);
       point && update.asks_depth < update.asks.size();
       point = price_level_index_.nextWorse(PriceLevelIndex::Side::Ask,
                                            point.price)) {
    const PooledPriceLevel *level = price_level_arena_.level(point.handle);
    if (level == nullptr) {
      break;
    }
    update.asks[update.asks_depth++] = {point.price, level->total_qty,
                                        level->num_orders};
  }
  return update;
}

const Order *OrderBook::getOrder(uint64_t order_id) const noexcept {
  const uint32_t order_idx = order_refs_.find(order_id);
  if (order_idx == LocalOrderRefMap::kInvalidIndex) {
    return nullptr;
  }
  const Order *order = order_arena_.at(order_idx);
  return order != nullptr && order->order_id == order_id ? order : nullptr;
}

OrderBookStats OrderBook::stats() const noexcept {
  return stats(BookStatsDetail::Full);
}

OrderBookStats OrderBook::stats(BookStatsDetail detail) const noexcept {
  OrderBookStats result{};
  result.invalid_adds = invalid_adds_;
  result.duplicate_order_refs = duplicate_order_refs_;
  result.missing_order_refs = missing_order_refs_;
  result.stale_order_refs = stale_order_refs_;
  result.mutation_failures = mutation_failures_;
  result.index_insert_failures = index_insert_failures_;
  result.index_replace_failures = index_replace_failures_;
  result.index_erase_failures = index_erase_failures_;
  result.price_rejections = price_rejections_;
  result.live_orders = live_order_count_;
  result.orders = order_arena_.stats();
  result.index_size = order_refs_.size();
  result.index_capacity = order_refs_.capacity();
  result.index_max_entries = order_refs_.maxEntries();
  result.index_failures = order_refs_.failureCounters();
  if (detail == BookStatsDetail::Full) {
    result.index_probes = order_refs_.probeStats();
  }
  return result;
}
