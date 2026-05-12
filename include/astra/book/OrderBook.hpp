#pragma once

#include "BookUpdate.hpp"
#include "TopOfBook.hpp"
#include "astra/book/Order.hpp"
#include "astra/book/OrderAction.hpp"
#include "astra/book/PriceLevel.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

class OrderBook {
public:
  explicit OrderBook(uint32_t symbol_id);

  void addOrder(const MarketDataMessageView &msg);
  void modifyOrder(const MarketDataMessageView &msg);
  void deleteOrder(const MarketDataMessageView &msg);
  void trade(const MarketDataMessageView &msg);

  TopOfBook getTopOfBook() const;
  BookUpdate getBookUpdate() const;

private:
  // order
  Order *allocateOrder();
  void freeOrder(Order *order);
  Order *findOrder(uint64_t order_id);

  // price level management
  PriceLevel *findPriceLevel(uint64_t price, OrderSide side);
  PriceLevel *findOrCreatePriceLevel(uint64_t price, OrderSide side);
  void removePriceLevelIfEmpty(uint64_t price, OrderSide side);
  void updatePriceLevelLinks(Order *order, PriceLevel *pl, OrderAction action);

  uint32_t symbol_id_;

  // Order pool
  static constexpr size_t kOrderPoolSize = 1 << 20; // 1 million orders
  std::deque<Order> order_pool_;
  std::vector<uint32_t> free_list_; // stack of free order indices

  // Price levels keyed by fixed-point price ticks.
  std::map<uint64_t, PriceLevel> bid_levels_;
  std::map<uint64_t, PriceLevel> ask_levels_;

  // Tracking orders by ID for quick access.
  // order_id -> order pool index
  std::unordered_map<uint64_t, uint32_t> order_by_id_;
};
