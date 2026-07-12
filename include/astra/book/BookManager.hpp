#pragma once

#include "astra/book/OrderBook.hpp"
#include "astra/book/OrderRefDirectory.hpp"
#include "astra/symbol/StockLocate.hpp"

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
  uint64_t directory_insert_failures{0};
  uint64_t directory_replace_failures{0};
  uint64_t directory_erase_failures{0};
  uint64_t late_book_creation_attempts{0};
  uint64_t allocation_failures{0};
  uint64_t price_rejections{0};
  uint64_t locally_invalid_books{0};
  size_t books{0};
  size_t live_orders{0};
  size_t directory_size{0};
  size_t directory_capacity{0};
  size_t directory_max_entries{0};
  size_t directory_max_probe_length{0};
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

  explicit BookManager(
      size_t order_directory_slots =
          OrderRefDirectory::kDefaultSlotCapacity);
  BookManager(size_t order_directory_slots,
              PriceLevelArenaConfig price_level_config);
  BookManager(size_t order_directory_slots,
              PriceLevelArenaConfig price_level_config,
              size_t order_pool_capacity);
  ~BookManager();

  static PriceLevelArenaConfig
  defaultPriceLevelArenaConfig(size_t order_pool_capacity) noexcept;

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
  void sealBookUniverse() noexcept { book_universe_sealed_ = true; }
  bool bookUniverseSealed() const noexcept { return book_universe_sealed_; }

  OrderBook *getOrCreate(uint16_t stock_locate) noexcept;
  const OrderBook *getOrderBook(uint16_t stock_locate) const noexcept;
  const Order *getOrder(uint64_t order_id) const noexcept;
  BookManagerStats stats() const noexcept;

private:
  OrderBook *find(uint16_t stock_locate) const noexcept;
  OrderBook *resolveOrderRef(uint16_t stock_locate, uint64_t order_id,
                             OrderRefDirectory::Handle &handle) noexcept;
  bool eraseStaleOrderRef(uint64_t order_id,
                          OrderRefDirectory::Handle handle) noexcept;
  bool eraseOrderRef(uint64_t order_id, OrderBook &book) noexcept;
  void recordMutationFailure(OrderBook &book) noexcept;

  OrderRefDirectory order_refs_;
  OrderArena orders_;
  PriceLevelArena price_levels_;
  std::array<std::unique_ptr<OrderBook>, kMaxStockLocate> books_{};

  uint64_t invalid_adds_{0};
  uint64_t duplicate_order_refs_{0};
  uint64_t missing_order_refs_{0};
  uint64_t stale_order_refs_{0};
  uint64_t mutation_failures_{0};
  uint64_t directory_insert_failures_{0};
  uint64_t directory_replace_failures_{0};
  uint64_t directory_erase_failures_{0};
  uint64_t late_book_creation_attempts_{0};
  bool book_universe_sealed_{false};
};
