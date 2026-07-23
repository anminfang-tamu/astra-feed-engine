#pragma once

#include "astra/book/BookUpdate.hpp"
#include "astra/book/MutationResult.hpp"
#include "astra/book/OrderTable.hpp"
#include "astra/book/PriceLevelStore.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/channel/ChannelHealth.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

class BookManager;

// A per-locate view over the process-wide order table and price-page arena.
// Managed books own no per-symbol hash table, FIFO, or price window.
class OrderBook {
public:
  // Focused standalone tests use this compatibility capacity. Production
  // books receive their process-wide capacities from BookManager and never
  // use this default. Keeping the standalone value bounded avoids reserving
  // tens of thousands of 2 MiB price pages in every unit-test process.
  static constexpr std::size_t kDefaultOrderPoolSize = 512u;

  // Compatibility/standalone constructor used by focused unit tests.  The
  // production path injects manager-owned process-wide stores.
  explicit OrderBook(std::uint32_t symbol_id,
                     std::size_t order_capacity = kDefaultOrderPoolSize);
  // Manager-backed view constructor. It allocates no storage and is public so
  // the manager's preallocated std::optional slots can construct it in place.
  OrderBook(std::uint16_t stock_locate, astra::book::OrderTable &orders,
            astra::book::PriceLevelStore &prices,
            std::size_t reported_capacity) noexcept;
  ~OrderBook();

  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;
  OrderBook(OrderBook &&) = delete;
  OrderBook &operator=(OrderBook &&) = delete;

  astra::book::MutationResult
  addOrder(std::uint64_t order_id, std::uint64_t price, std::uint32_t qty,
           char side) noexcept;
  astra::book::MutationResult cancelShares(std::uint64_t order_id,
                                           std::uint32_t canceled_qty) noexcept;
  astra::book::MutationResult deleteOrder(std::uint64_t order_id) noexcept;
  astra::book::MutationResult executeOrder(std::uint64_t order_id,
                                           std::uint32_t executed_qty) noexcept;
  bool trade(std::uint64_t order_id, std::uint32_t executed_qty) noexcept {
    return executeOrder(order_id, executed_qty) ==
           astra::book::MutationResult::Applied;
  }
  astra::book::MutationResult
  replaceOrder(std::uint64_t old_id, std::uint64_t new_id,
               std::uint64_t new_price, std::uint32_t new_qty) noexcept;

  TopOfBook getTopOfBook() const noexcept;
  BookUpdate getBookUpdate() const noexcept;

  std::uint32_t symbolId() const noexcept { return symbol_id_; }
  std::size_t orderCapacity() const noexcept { return order_capacity_; }
  std::size_t liveOrderCount() const noexcept { return live_order_count_; }
  std::uint64_t allocationFailures() const noexcept {
    return allocation_failures_;
  }
  std::uint16_t stockLocate() const noexcept { return stock_locate_; }
  bool localInvalid() const noexcept { return local_invalid_; }
  bool isTradable(ChannelHealth channel_health) const noexcept {
    return channel_health == ChannelHealth::Good && !local_invalid_;
  }

private:
  struct OwnedStores;

  static OrderSide decodeSide(char side) noexcept;
  static OrderSide decodeSide(std::uint8_t side) noexcept;
  static astra::book::PriceAddress
  addressOf(const astra::book::OrderState &state) noexcept;
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((cold, noinline))
#endif
  astra::book::MutationResult
  reject(astra::book::MutationResult result) noexcept;
  astra::book::MutationResult
  validateState(astra::book::OrderState *state) noexcept;

  std::unique_ptr<OwnedStores> owned_stores_;
  astra::book::OrderTable *orders_{nullptr};
  astra::book::PriceLevelStore *prices_{nullptr};
  std::uint32_t symbol_id_{0};
  std::uint16_t stock_locate_{0};
  bool local_invalid_{false};
  std::size_t order_capacity_{0};
  std::size_t live_order_count_{0};
  std::uint64_t allocation_failures_{0};
};
