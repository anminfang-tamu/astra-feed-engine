#pragma once

#include "astra/book/BookCapacity.hpp"
#include "astra/book/OrderBook.hpp"
#include "astra/symbol/StockLocate.hpp"
#include "astra/symbol/SymbolTier.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

// Routes ITCH book operations to the correct OrderBook by Stock Locate code.
// Stock Locate codes (uint16, 1-8191) are assigned by NASDAQ per symbol and
// are present in every ITCH message, so no secondary lookup is needed.
class BookManager {
public:
  static constexpr uint16_t kMaxStockLocate = astra::symbol::kMaxStockLocate;

  BookManager();
  ~BookManager();

  void setBookCapacityTier(uint16_t stock_locate,
                           astra::symbol::SymbolTier tier) noexcept;
  void prepareBook(uint16_t stock_locate, bool touch_pages = false) noexcept;
  void addOrder(uint16_t stock_locate, uint64_t order_id, uint64_t price,
                uint32_t qty, char side) noexcept;
  void cancelShares(uint16_t stock_locate, uint64_t order_id,
                    uint32_t canceled_qty) noexcept;
  void deleteOrder(uint16_t stock_locate, uint64_t order_id) noexcept;
  void trade(uint16_t stock_locate, uint64_t order_id,
             uint32_t executed_qty) noexcept;
  void replaceOrder(uint16_t stock_locate, uint64_t old_id, uint64_t new_id,
                    uint64_t new_price, uint32_t new_qty) noexcept;
  void reverseExecution(uint16_t stock_locate, uint64_t order_id,
                        uint64_t price, uint32_t qty, char side) noexcept;

  const OrderBook *getOrderBook(uint16_t stock_locate) const noexcept;

private:
  OrderBook *getOrCreate(uint16_t stock_locate) noexcept;
  OrderBook *find(uint16_t stock_locate) const noexcept;
  size_t orderCapacityForLocate(uint16_t stock_locate) const noexcept;

  std::array<size_t, kMaxStockLocate> order_capacity_by_locate_{};
  std::array<std::unique_ptr<OrderBook>, kMaxStockLocate> books_{};
};
