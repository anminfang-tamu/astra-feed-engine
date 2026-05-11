#pragma once

#include "BookUpdate.hpp"
#include "TopOfBook.hpp"
#include "astra/book/Order.hpp"
#include "astra/book/OrderAction.hpp"
#include "astra/book/PriceLevel.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/protocol/OrderSide.hpp"
#include "astra/utils/Bitmap.hpp"

#include "absl/container/flat_hash_map.h"

#include <cstddef>
#include <cstdint>
#include <limits>
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
  PriceLevel *findPriceLevel(uint32_t price, OrderSide side);
  PriceLevel *findOrCreatePriceLevel(uint32_t price, OrderSide side);
  void removePriceLevelIfEmpty(uint32_t price, OrderSide side);
  void updatePriceLevelLinks(Order *order, PriceLevel *pl, OrderAction action);
  uint32_t priceIndex(uint32_t price) const;
  void markPriceLevelActive(uint32_t price_index, OrderSide side);
  void markPriceLevelInactive(uint32_t price_index, OrderSide side);

  // best bid/ask management
  void updateBestOnAdd(uint32_t price_index, OrderSide side);
  void updateBestOnRemove(uint32_t price_index, OrderSide side);

  uint32_t symbol_id_;

  // Order pool
  static constexpr size_t kOrderPoolSize = 1 << 20; // 1 million orders
  static constexpr uint32_t kPriceScale = 10000;
  static constexpr uint32_t kNoPriceIndex =
      std::numeric_limits<uint32_t>::max();
  std::vector<Order> order_pool_;
  std::vector<uint32_t> free_list_; // stack of free order indices

  // Price levels for bids and asks, indexed by normalized price.
  std::vector<PriceLevel> bid_levels_;
  std::vector<PriceLevel> ask_levels_;

  Bitmap bid_bitmap_;
  Bitmap ask_bitmap_;

  // Best bid and ask are normalized price indexes.
  uint32_t best_bid_;
  uint32_t best_ask_;

  // Tracking orders by ID for quick access.
  // order_id -> order pool index
  absl::flat_hash_map<uint64_t, uint32_t> order_by_id_;
};
