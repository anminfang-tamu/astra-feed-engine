#include "astra/book/OrderBook.hpp"
#include "astra/book/OrderAction.hpp"

OrderBook::OrderBook(uint32_t symbol_id) : symbol_id_(symbol_id) {}

Order *OrderBook::allocateOrder() {
  if (!free_list_.empty()) {
    const uint32_t index = free_list_.back();
    free_list_.pop_back();
    Order *order = &order_pool_[index];
    order->active = true;
    return order;
  }

  if (order_pool_.size() >= kOrderPoolSize) {
    return nullptr;
  }

  const uint32_t index = static_cast<uint32_t>(order_pool_.size());
  order_pool_.push_back(Order{});
  Order *order = &order_pool_.back();
  order->pool_index = index;
  order->active = true;
  return order;
}

void OrderBook::freeOrder(Order *order) {
  order->active = false;
  order_by_id_.erase(order->order_id);
  order->prev = nullptr;
  order->next = nullptr;
  free_list_.push_back(order->pool_index);
}

Order *OrderBook::findOrder(uint64_t order_id) {
  const auto it = order_by_id_.find(order_id);
  if (it == order_by_id_.end()) {
    return nullptr;
  }

  const uint32_t order_index = it->second;
  if (order_index >= order_pool_.size()) {
    return nullptr;
  }

  Order *order = &order_pool_[order_index];
  return order->active ? order : nullptr;
}

PriceLevel *OrderBook::findPriceLevel(uint64_t price, OrderSide side) {
  std::map<uint64_t, PriceLevel> &levels =
      (side == OrderSide::Buy) ? bid_levels_ : ask_levels_;
  const auto it = levels.find(price);
  if (it == levels.end() || it->second.num_orders == 0) {
    return nullptr;
  }
  return &it->second;
}

PriceLevel *OrderBook::findOrCreatePriceLevel(uint64_t price, OrderSide side) {
  std::map<uint64_t, PriceLevel> &levels =
      (side == OrderSide::Buy) ? bid_levels_ : ask_levels_;
  const auto [it, inserted] = levels.try_emplace(price);
  PriceLevel &pl = it->second;
  if (inserted) {
    pl.price = price;
    pl.qty = 0;
    pl.head = nullptr;
    pl.tail = nullptr;
  }
  return &pl;
}

void OrderBook::removePriceLevelIfEmpty(uint64_t price, OrderSide side) {
  std::map<uint64_t, PriceLevel> &levels =
      (side == OrderSide::Buy) ? bid_levels_ : ask_levels_;
  const auto it = levels.find(price);
  if (it == levels.end()) {
    return;
  }
  if (it->second.qty == 0) {
    levels.erase(it);
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

void OrderBook::addOrder(const MarketDataMessageView &msg) {
  if (msg.qty() == 0 || findOrder(msg.orderId()) != nullptr) {
    return;
  }

  // allocate a new order from the pool
  Order *order = allocateOrder();
  if (order == nullptr) {
    return;
  }

  order->order_id = msg.orderId();
  order->symbol_id = symbol_id_;
  order->price = msg.price();
  order->qty = msg.qty();
  order->side = msg.side();
  order->active = true;
  order->prev = nullptr;
  order->next = nullptr;

  // insert into price level
  PriceLevel *pl = findOrCreatePriceLevel(order->price, order->side);
  updatePriceLevelLinks(order, pl, OrderAction::Add);
  pl->qty += order->qty;
  pl->num_orders += 1;
  // track order by ID
  order_by_id_.insert_or_assign(order->order_id, order->pool_index);
}

void OrderBook::modifyOrder(const MarketDataMessageView &msg) {
  Order *order = findOrder(msg.orderId());
  if (order == nullptr) {
    return;
  }

  if (msg.qty() == 0) {
    deleteOrder(msg);
    return;
  }

  uint64_t old_price = order->price;
  uint32_t old_qty = order->qty;
  OrderSide old_side = order->side;
  uint64_t new_price = msg.price();
  uint32_t new_qty = msg.qty();
  OrderSide new_side = msg.side();

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
  } else if (old_qty != new_qty) {
    old_pl->qty = old_pl->qty - old_qty + new_qty;
    order->qty = new_qty;
  }
}

void OrderBook::deleteOrder(const MarketDataMessageView &msg) {
  Order *order = findOrder(msg.orderId());
  if (order == nullptr) {
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

void OrderBook::trade(const MarketDataMessageView &msg) {
  if (msg.qty() == 0) {
    return;
  }

  Order *order = findOrder(msg.orderId());
  if (order == nullptr) {
    return;
  }
  if (msg.qty() > order->qty) {
    return;
  }

  PriceLevel *pl = findPriceLevel(order->price, order->side);
  if (pl == nullptr || pl->qty < msg.qty() || pl->num_orders == 0) {
    return;
  }
  pl->qty -= msg.qty();
  if (msg.qty() >= order->qty) {
    pl->num_orders -= 1;
    updatePriceLevelLinks(order, pl, OrderAction::Delete);
    removePriceLevelIfEmpty(order->price, order->side);
    freeOrder(order);
  } else {
    order->qty -= msg.qty();
  }
}

TopOfBook OrderBook::getTopOfBook() const {
  TopOfBook top{};
  top.symbol_id = symbol_id_;

  if (!bid_levels_.empty()) {
    const PriceLevel &bid = bid_levels_.rbegin()->second;
    top.bid_price = bid.price;
    top.bid_qty = bid.qty;
  }

  if (!ask_levels_.empty()) {
    const PriceLevel &ask = ask_levels_.begin()->second;
    top.ask_price = ask.price;
    top.ask_qty = ask.qty;
  }

  return top;
}

BookUpdate OrderBook::getBookUpdate() const {
  BookUpdate update{};
  update.symbol_id = symbol_id_;

  for (auto it = bid_levels_.rbegin();
       it != bid_levels_.rend() && update.bids_depth < update.bids.size();
       ++it) {
    update.bids[update.bids_depth] = it->second;
    ++update.bids_depth;
  }

  for (auto it = ask_levels_.begin();
       it != ask_levels_.end() && update.asks_depth < update.asks.size();
       ++it) {
    update.asks[update.asks_depth] = it->second;
    ++update.asks_depth;
  }

  return update;
}
