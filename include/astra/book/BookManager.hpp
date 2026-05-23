#pragma once

#include "astra/book/OrderBook.hpp"
#include "astra/constants/SymbolCapacity.hpp"
#include "astra/protocol/OrderSide.hpp"
#include "astra/protocol/SymbolTable.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

// Routes ITCH book operations to the correct OrderBook by Stock Locate code.
// Stock Locate codes (uint16, 1-8191) are assigned by NASDAQ per symbol and
// are present in every ITCH message, so no secondary lookup is needed.
class BookManager {
public:
  static constexpr uint16_t kMaxStockLocate = SymbolTable::kMaxLocate;

  BookManager();
  ~BookManager();

  void registerStockLocate(uint16_t stock_locate, uint16_t channel_id) noexcept;
  void setSymbolTier(uint16_t stock_locate,
                     astra::capacity::SymbolTier tier) noexcept;
  void setSymbolOrderCapacity(uint16_t stock_locate,
                              size_t order_capacity) noexcept;
  void addOrder(uint16_t stock_locate, uint64_t order_id, uint64_t price,
                uint32_t qty, OrderSide side,
                uint16_t channel_id = 0) noexcept;
  void cancelShares(uint16_t stock_locate, uint64_t order_id,
                    uint32_t canceled_qty) noexcept;
  void deleteOrder(uint16_t stock_locate, uint64_t order_id) noexcept;
  void trade(uint16_t stock_locate, uint64_t order_id,
             uint32_t executed_qty) noexcept;
  void replaceOrder(uint16_t stock_locate, uint64_t old_id, uint64_t new_id,
                    uint64_t new_price, uint32_t new_qty) noexcept;
  void reverseExecution(uint16_t stock_locate, uint64_t order_id,
                        uint64_t price, uint32_t qty, OrderSide side) noexcept;

  const OrderBook *getOrderBook(uint16_t stock_locate) const noexcept;

private:
  OrderBook *getOrCreate(uint16_t stock_locate) noexcept;
  OrderBook *getOrCreate(uint16_t stock_locate, uint16_t channel_id) noexcept;
  OrderBook *find(uint16_t stock_locate) const noexcept;

  std::array<OrderBook *, kMaxStockLocate> book_by_locate_{};
  std::array<uint16_t, kMaxStockLocate> channel_by_locate_{};
  std::array<size_t, kMaxStockLocate> order_capacity_by_locate_{};
  std::array<bool, kMaxStockLocate> locate_registered_{};
  std::array<std::unique_ptr<OrderBook>, kMaxStockLocate> books_{};
  uint16_t active_book_count_{0};
};
