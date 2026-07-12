#pragma once

#include "BookUpdate.hpp"
#include "TopOfBook.hpp"
#include "astra/book/Order.hpp"
#include "astra/book/OrderArena.hpp"
#include "astra/book/PriceLevelIndex.hpp"
#include "astra/channel/ChannelHealth.hpp"

#include <cstddef>
#include <cstdint>

class OrderBook {
public:
  static constexpr uint32_t kInvalidIdx = UINT32_MAX;

  explicit OrderBook(uint32_t symbol_id, OrderArena &order_arena,
                     PriceLevelArena &price_level_arena);

  TopOfBook getTopOfBook() const noexcept;
  BookUpdate getBookUpdate() const noexcept;

  uint32_t symbolId() const noexcept { return symbol_id_; }
  size_t liveOrderCount() const noexcept { return live_order_count_; }
  uint64_t priceRejections() const noexcept { return price_rejections_; }
  uint16_t stockLocate() const noexcept { return stock_locate_; }
  bool localInvalid() const noexcept { return local_invalid_; }
  void markLocalInvalid() noexcept { local_invalid_ = true; }
  void clearLocalInvalid() noexcept { local_invalid_ = false; }
  bool isTradable(ChannelHealth channel_health) const noexcept {
    return channel_health == ChannelHealth::Good && !local_invalid_;
  }

private:
  friend class BookManager;

  enum class MutationResult : uint8_t { Ignored, Updated, Removed };

  uint32_t allocateOrder() noexcept;
  void freeOrder(uint32_t order_idx) noexcept;
  bool removeFromLevel(uint32_t order_idx) noexcept;
  bool hasOrderAt(uint32_t order_idx, uint64_t order_id) const noexcept;
  const Order *orderAt(uint32_t order_idx) const noexcept;
  uint32_t addOrderIndexed(uint64_t order_id, uint64_t price, uint32_t qty,
                           char side) noexcept;
  MutationResult cancelSharesIndexed(uint32_t order_idx, uint64_t order_id,
                                     uint32_t canceled_qty) noexcept;
  bool deleteOrderIndexed(uint32_t order_idx, uint64_t order_id) noexcept;
  MutationResult tradeIndexed(uint32_t order_idx, uint64_t order_id,
                              uint32_t executed_qty) noexcept;
  MutationResult replaceOrderIndexed(uint32_t order_idx, uint64_t old_id,
                                     uint64_t new_id, uint64_t new_price,
                                     uint32_t new_qty) noexcept;
  MutationResult restoreSharesIndexed(uint32_t order_idx, uint64_t order_id,
                                      uint32_t qty) noexcept;

  uint32_t symbol_id_;
  OrderArena &order_arena_;
  PriceLevelArena &price_level_arena_;
  PriceLevelIndex price_level_index_;
  size_t live_order_count_{0};

  uint16_t stock_locate_;
  uint64_t price_rejections_{0};
  bool local_invalid_{false};
};
