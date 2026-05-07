#include "astra/book/OrderBook.hpp"
#include "astra/book/OrderAction.hpp"

OrderBook::OrderBook(uint32_t symbol_id)
    : symbol_id_(symbol_id), order_pool_(kOrderPoolSize),
      best_bid_(kNoPriceIndex), best_ask_(kNoPriceIndex) {
  // Initialize
  for (uint32_t i = 0; i < kOrderPoolSize; ++i) {
    free_list_.push_back(i);
    order_pool_[i].active = false;
  }
  order_by_id_.reserve(kOrderPoolSize);
  order_by_id_.max_load_factor(0.7f);
}

Order *OrderBook::allocateOrder() {
  uint32_t index = free_list_.back();
  free_list_.pop_back();
  Order *order = &order_pool_[index];
  order->active = true;
  return order;
}

void OrderBook::freeOrder(Order *order) {
  order->active = false;
  uint32_t index = order - &order_pool_[0];
  order_by_id_.erase(order->order_id);
  order->prev = nullptr;
  order->next = nullptr;
  free_list_.push_back(index);
}

uint32_t OrderBook::priceIndex(uint32_t price) const {
  return price * kPriceScale;
}

void OrderBook::markPriceLevelActive(uint32_t price_index, OrderSide side) {
  Bitmap &bitmap = (side == OrderSide::Buy) ? bid_bitmap_ : ask_bitmap_;
  bitmap.set(price_index);
}

void OrderBook::markPriceLevelInactive(uint32_t price_index, OrderSide side) {
  Bitmap &bitmap = (side == OrderSide::Buy) ? bid_bitmap_ : ask_bitmap_;
  bitmap.reset(price_index);
}

void OrderBook::updateBestOnAdd(uint32_t price_index, OrderSide side) {
  if (side == OrderSide::Buy) {
    if (best_bid_ == kNoPriceIndex || price_index > best_bid_) {
      best_bid_ = price_index;
    }
  } else if (best_ask_ == kNoPriceIndex || price_index < best_ask_) {
    best_ask_ = price_index;
  }
}

void OrderBook::updateBestOnRemove(uint32_t price_index, OrderSide side) {
  if (side == OrderSide::Buy) {
    if (price_index == best_bid_) {
      best_bid_ = price_index == 0
                      ? kNoPriceIndex
                      : bid_bitmap_.findPrevSet(price_index - 1);
    }
  } else if (price_index == best_ask_) {
    best_ask_ = ask_bitmap_.findNextSet(price_index + 1);
  }
}

PriceLevel *OrderBook::findPriceLevel(uint32_t price, OrderSide side) {
  std::vector<PriceLevel> &levels =
      (side == OrderSide::Buy) ? bid_levels_ : ask_levels_;
  uint32_t target_price = priceIndex(price);
  if (target_price >= levels.size()) {
    return nullptr;
  }
  PriceLevel &pl = levels[target_price];
  return pl.num_orders == 0 ? nullptr : &pl;
}

PriceLevel *OrderBook::findOrCreatePriceLevel(uint32_t price, OrderSide side) {
  std::vector<PriceLevel> &levels =
      (side == OrderSide::Buy) ? bid_levels_ : ask_levels_;
  uint32_t target_price = priceIndex(price);
  if (target_price >= levels.size()) {
    levels.resize(static_cast<size_t>(target_price) + 1);
  }
  PriceLevel &pl = levels[target_price];
  if (pl.num_orders == 0) {
    pl.price = price;
    pl.qty = 0;
    pl.head = nullptr;
    pl.tail = nullptr;
  }
  return &pl;
}

void OrderBook::removePriceLevelIfEmpty(uint32_t price, OrderSide side) {
  std::vector<PriceLevel> &levels =
      (side == OrderSide::Buy) ? bid_levels_ : ask_levels_;
  uint32_t target_price = priceIndex(price);
  if (target_price >= levels.size()) {
    return;
  }
  PriceLevel &pl = levels[target_price];
  if (pl.qty == 0) {
    pl = PriceLevel{};
    markPriceLevelInactive(target_price, side);
    updateBestOnRemove(target_price, side);
  }
}

void OrderBook::updatePriceLevelLinks(Order *order, PriceLevel *pl,
                                      OrderAction action) {
  if (action == OrderAction::Add) {
    if (pl->head == nullptr) {
      pl->head = order;
      pl->tail = order;
    } else {
      pl->tail->next = order;
      order->prev = pl->tail;
      pl->tail = order;
    }
  } else if (action == OrderAction::Delete) {
    if (order->prev) {
      order->prev->next = order->next;
    } else {
      pl->head = order->next;
    }
    if (order->next) {
      order->next->prev = order->prev;
    } else {
      pl->tail = order->prev;
    }
  }
}

void OrderBook::addOrder(const MarketDataMessage &msg) {
  // allocate a new order from the pool
  Order *order = allocateOrder();
  order->order_id = msg.order_id;
  order->symbol_id = symbol_id_;
  order->price = msg.price;
  order->qty = msg.qty;
  order->side = msg.side;
  order->active = true;
  order->prev = nullptr;
  order->next = nullptr;

  // insert into price level
  PriceLevel *pl = findOrCreatePriceLevel(order->price, order->side);
  updatePriceLevelLinks(order, pl, OrderAction::Add);
  pl->qty += order->qty;
  pl->num_orders += 1;
  const uint32_t target_price = priceIndex(order->price);
  markPriceLevelActive(target_price, order->side);
  updateBestOnAdd(target_price, order->side);

  // track order by ID
  order_by_id_[order->order_id] = order - &order_pool_[0];
}

void OrderBook::modifyOrder(const MarketDataMessage &msg) {
  const auto it = order_by_id_.find(msg.order_id);
  if (it == order_by_id_.end()) {
    // Order not found, ignore or log error
    return;
  } else {
    Order *order = &order_pool_[it->second];
    if (!order->active) {
      // Order is not active, ignore or log error
      return;
    }

    uint32_t old_price = order->price;
    uint32_t old_qty = order->qty;
    OrderSide old_side = order->side;
    uint32_t new_price = msg.price;
    uint32_t new_qty = msg.qty;
    OrderSide new_side = msg.side;

    PriceLevel *old_pl = findPriceLevel(old_price, old_side);
    if (old_pl == nullptr || old_pl->qty < old_qty || old_pl->num_orders == 0) {
      return;
    }

    if (old_price != new_price || old_side != new_side) {
      old_pl->qty -= old_qty;
      old_pl->num_orders -= 1;

      // unlink order from old price level
      updatePriceLevelLinks(order, old_pl, OrderAction::Delete);

      removePriceLevelIfEmpty(old_price, old_side);

      PriceLevel *new_pl = findOrCreatePriceLevel(new_price, new_side);
      // update order
      order->price = new_price;
      order->qty = new_qty;
      order->side = new_side;
      // insert into new price level
      order->prev = new_pl->tail;
      order->next = nullptr;
      // update price level
      updatePriceLevelLinks(order, new_pl, OrderAction::Add);
      new_pl->qty += new_qty;
      new_pl->num_orders += 1;
      const uint32_t new_target_price = priceIndex(new_price);
      markPriceLevelActive(new_target_price, new_side);
      updateBestOnAdd(new_target_price, new_side);
    } else if (old_qty != new_qty) {
      old_pl->qty = old_pl->qty - old_qty + new_qty;
      order->qty = new_qty;
    }
  }
}

void OrderBook::deleteOrder(const MarketDataMessage &msg) {
  const auto it = order_by_id_.find((msg.order_id));
  if (it == order_by_id_.end()) {
    return;
  }
  Order *order = &order_pool_[it->second];
  if (!order->active) {
    return;
  }
  PriceLevel *pl = findPriceLevel(order->price, order->side);
  if (pl == nullptr || pl->qty < order->qty || pl->num_orders == 0) {
    return;
  }
  pl->qty -= order->qty;
  pl->num_orders -= 1;
  updatePriceLevelLinks(order, pl, OrderAction::Delete);
  removePriceLevelIfEmpty(order->price, order->side);
  freeOrder(order);
}

TopOfBook OrderBook::getTopOfBook() const {
  TopOfBook top{};
  top.symbol_id = symbol_id_;

  if (best_bid_ != kNoPriceIndex) {
    const PriceLevel &bid = bid_levels_[best_bid_];
    top.bid_price = bid.price;
    top.bid_qty = bid.qty;
  }

  if (best_ask_ != kNoPriceIndex) {
    const PriceLevel &ask = ask_levels_[best_ask_];
    top.ask_price = ask.price;
    top.ask_qty = ask.qty;
  }

  return top;
}

BookUpdate OrderBook::getBookUpdate() const {
  BookUpdate update{};
  update.symbol_id = symbol_id_;

  uint32_t bid_index = best_bid_;
  while (bid_index != kNoPriceIndex && update.bids_depth < update.bids.size()) {
    update.bids[update.bids_depth] = bid_levels_[bid_index];
    ++update.bids_depth;

    if (bid_index == 0) {
      break;
    }

    bid_index = bid_bitmap_.findPrevSet(bid_index - 1);
  }

  uint32_t ask_index = best_ask_;
  while (ask_index != kNoPriceIndex && update.asks_depth < update.asks.size()) {
    update.asks[update.asks_depth] = ask_levels_[ask_index];
    ++update.asks_depth;

    if (ask_index == kNoPriceIndex - 1) {
      break;
    }

    ask_index = ask_bitmap_.findNextSet(ask_index + 1);
  }

  return update;
}
