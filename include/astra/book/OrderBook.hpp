#pragma once

#include "BookUpdate.hpp"
#include "TopOfBook.hpp"
#include "astra/book/BookCapacity.hpp"
#include "astra/book/Order.hpp"
#include "astra/book/PriceLevel.hpp"
#include "astra/channel/ChannelHealth.hpp"
#include "astra/utils/FixedMmapArray.hpp"
#include "astra/utils/OrderIdMap.hpp"
#include "astra/utils/PriceBitmap.hpp"

#include <cstddef>
#include <cstdint>

class OrderBook {
public:
  static constexpr uint32_t kInvalidIdx = UINT32_MAX;
  static constexpr size_t kMaxPriceLevels = 1 << 16; // 65 536 levels per side
  static constexpr size_t kDefaultOrderPoolSize =
      astra::book_capacity::kDefaultOrderCapacity;

  explicit OrderBook(uint32_t symbol_id,
                     size_t order_capacity = kDefaultOrderPoolSize);

  void addOrder(uint64_t order_id, uint64_t price, uint32_t qty,
                char side) noexcept;
  void cancelShares(uint64_t order_id, uint32_t canceled_qty) noexcept;
  void deleteOrder(uint64_t order_id) noexcept;
  void trade(uint64_t order_id, uint32_t executed_qty) noexcept;
  void replaceOrder(uint64_t old_id, uint64_t new_id, uint64_t new_price,
                    uint32_t new_qty) noexcept;
  void reverseExecution(uint64_t order_id, uint64_t price, uint32_t qty,
                        char side) noexcept;
  void warmStorage() noexcept;

  TopOfBook getTopOfBook() const noexcept;
  BookUpdate getBookUpdate() const noexcept;

  uint32_t symbolId() const noexcept { return symbol_id_; }
  size_t orderCapacity() const noexcept { return order_capacity_; }
  size_t liveOrderCount() const noexcept {
    return next_order_idx_ - free_list_size_;
  }
  uint64_t allocationFailures() const noexcept { return allocation_failures_; }
  uint16_t stockLocate() const noexcept { return stock_locate_; }
  bool localInvalid() const noexcept { return local_invalid_; }
  void markLocalInvalid() noexcept { local_invalid_ = true; }
  void clearLocalInvalid() noexcept { local_invalid_ = false; }
  bool isTradable(ChannelHealth channel_health) const noexcept {
    return channel_health == ChannelHealth::Good && !local_invalid_;
  }

private:
  uint32_t priceToIndex(uint64_t price) const noexcept;
  uint32_t allocateOrder() noexcept;
  void freeOrder(uint32_t order_idx) noexcept;
  bool removeFromLevel(uint32_t order_idx) noexcept;

  uint32_t symbol_id_;
  size_t order_capacity_;
  uint64_t reference_price_;
  uint64_t tick_size_;

  alignas(64) FixedMmapArray<Order> order_pool_;
  FixedMmapArray<uint32_t> free_list_;
  size_t free_list_size_;
  size_t next_order_idx_;

  FixedMmapArray<PriceLevel> bid_levels_;
  FixedMmapArray<PriceLevel> ask_levels_;

  PriceBitmap bid_bitmap_;
  PriceBitmap ask_bitmap_;

  OrderIdMap order_index_;

  uint16_t stock_locate_;
  uint64_t allocation_failures_{0};
  bool local_invalid_{false};
};
