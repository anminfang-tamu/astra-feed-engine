#pragma once

#include "BookUpdate.hpp"
#include "TopOfBook.hpp"
#include "astra/book/Order.hpp"
#include "astra/book/PriceLevel.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/utils/OrderIdMap.hpp"
#include "astra/utils/PriceBitmap.hpp"

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <vector>

class OrderBook {
public:
  static constexpr uint32_t kInvalidIdx = UINT32_MAX;
  static constexpr size_t kOrderPoolSize = 1 << 20; // 1 million orders
  static constexpr size_t kMaxPriceLevels =
      1 << 16; // 65,536 price levels per side

  explicit OrderBook(uint32_t symbol_id);

  void addOrder(const MarketDataMessageView &msg) noexcept;
  void modifyOrder(const MarketDataMessageView &msg) noexcept;
  void deleteOrder(const MarketDataMessageView &msg) noexcept;
  void trade(const MarketDataMessageView &msg) noexcept;

  TopOfBook getTopOfBook() const noexcept;
  BookUpdate getBookUpdate() const noexcept;

private:
  uint32_t priceToIndex(uint64_t price) const noexcept;
  uint32_t allocateOrder() noexcept;
  void freeOrder(uint32_t order_idx) noexcept;

  uint32_t symbol_id_;
  uint64_t reference_price_;
  uint64_t tick_size_;

  // Order Arena
  alignas(64) std::vector<Order> order_pool_;
  std::vector<uint32_t> free_list_; // stack of free order indices

  // Price levels - dense arrays
  std::vector<PriceLevel> bid_levels_;
  std::vector<PriceLevel> ask_levels_;

  PriceBitmap bid_bitmap_;
  PriceBitmap ask_bitmap_;

  OrderIdMap order_id_map_; // maps order_id to order_idx in order_pool_
};
