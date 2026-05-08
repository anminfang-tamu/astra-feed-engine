#pragma once

#include "OrderBook.hpp"
#include "astra/protocol/MarketDataMessage.hpp"

#include <cstdint>
#include <memory>
#include <vector>

class BookManager {
public:
  BookManager();
  ~BookManager();

  void onMessage(const MarketDataMessage &message);
  const OrderBook *getOrderBook(uint32_t symbol_id) const;

private:
  OrderBook *findOrderBook(uint32_t symbol_id);
  OrderBook *getOrCreateOrderBook(uint32_t symbol_id);

  std::vector<std::unique_ptr<OrderBook>> books_;
  std::vector<OrderBook *> book_by_symbol_id_;
};
