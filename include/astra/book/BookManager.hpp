#pragma once

#include "astra/book/BookCapacity.hpp"
#include "astra/book/OrderBook.hpp"
#include "astra/symbol/StockLocate.hpp"
#include "astra/symbol/SymbolTier.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

struct BookManagerStats {
  uint64_t invalid_adds{0};
  uint64_t duplicate_order_refs{0};
  uint64_t missing_order_refs{0};
  uint64_t stale_order_refs{0};
  uint64_t mutation_failures{0};
  uint64_t order_ref_index_insert_failures{0};
  uint64_t order_ref_index_replace_failures{0};
  uint64_t order_ref_index_erase_failures{0};
  uint64_t late_book_creation_attempts{0};
  uint64_t book_creation_failures{0};
  uint64_t allocation_failures{0};
  uint64_t price_rejections{0};
  uint64_t locally_invalid_books{0};
  uint64_t inconsistent_books{0};
  size_t books{0};
  size_t live_orders{0};
  size_t live_order_high_watermark{0};
  size_t order_ref_index_size{0};
  size_t order_ref_index_capacity{0};
  size_t order_ref_index_max_entries{0};
  size_t order_ref_index_max_probe_length{0};
  OrderArenaStats orders{};
  PriceLevelArenaStats price_levels{};
};

// Routes ITCH book operations to the correct OrderBook by Stock Locate code.
// Stock Locate codes (uint16, 1-16383) are assigned by NASDAQ per symbol and
// are present in every ITCH message, so no secondary lookup is needed.
class BookManager {
public:
  static constexpr uint16_t kMaxStockLocate = astra::symbol::kMaxStockLocate;
  static constexpr size_t kDefaultPriceInternalNodeCapacity = 163'840;
  static constexpr size_t kDefaultPriceLeafCapacity = 1'048'576;
  static constexpr size_t kDefaultPriceLevelCapacity = 2'097'152;

  BookManager();
  BookManager(size_t default_order_capacity,
              PriceLevelArenaConfig price_level_config);
  ~BookManager();

  void addOrder(uint16_t stock_locate, uint64_t order_id, uint64_t price,
                uint32_t qty, char side) noexcept;
  void cancelShares(uint16_t stock_locate, uint64_t order_id,
                    uint32_t canceled_qty) noexcept;
  void deleteOrder(uint16_t stock_locate, uint64_t order_id) noexcept;
  bool trade(uint16_t stock_locate, uint64_t order_id,
             uint32_t executed_qty) noexcept;
  void replaceOrder(uint16_t stock_locate, uint64_t old_id, uint64_t new_id,
                    uint64_t new_price, uint32_t new_qty) noexcept;
  void reverseExecution(uint16_t stock_locate, uint64_t order_id,
                        uint64_t price, uint32_t qty, char side) noexcept;
  bool prepareBook(uint16_t stock_locate) noexcept;
  void sealBookUniverse() noexcept { book_universe_sealed_ = true; }
  bool bookUniverseSealed() const noexcept { return book_universe_sealed_; }

  void setBookCapacityTier(uint16_t stock_locate,
                           astra::symbol::SymbolTier tier) noexcept;
  void setBookOrderCapacity(uint16_t stock_locate,
                            size_t order_capacity) noexcept;
  size_t orderCapacityForLocate(uint16_t stock_locate) const noexcept;

  const OrderBook *getOrderBook(uint16_t stock_locate) const noexcept;
  const Order *getOrder(uint16_t stock_locate,
                        uint64_t order_id) const noexcept;
  // Aggregates every book and scans each local reference table to report its
  // current probe layout. This is a shutdown/offline diagnostic, not a hot-
  // path or periodic metrics call.
  BookManagerStats stats() const noexcept;

private:
  OrderBook *find(uint16_t stock_locate) const noexcept;
  OrderBook *getOrCreate(uint16_t stock_locate) noexcept;
  void observeLiveOrderChange(size_t before, size_t after) noexcept;

  size_t default_order_capacity_;
  PriceLevelArena price_levels_;
  std::array<std::unique_ptr<OrderBook>, kMaxStockLocate> books_{};
  std::array<size_t, kMaxStockLocate> order_capacity_by_locate_{};
  std::array<bool, kMaxStockLocate> explicit_order_capacity_by_locate_{};
  size_t live_orders_{0};
  size_t live_order_high_watermark_{0};

  uint64_t invalid_adds_{0};
  uint64_t missing_order_refs_{0};
  uint64_t mutation_failures_{0};
  uint64_t late_book_creation_attempts_{0};
  uint64_t book_creation_failures_{0};
  bool book_universe_sealed_{false};
};
