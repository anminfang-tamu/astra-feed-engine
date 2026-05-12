#pragma once

#include <cstdint>

#include "Order.hpp"

struct PriceLevel {
  uint64_t price;      // 8 bytes, fixed-point ticks
  uint32_t qty;        // 4 bytes, total quantity at this price level
  uint32_t num_orders; // 4 bytes, number of orders at this price level

  Order *head; // pointer to the head of the linked list of orders at this price
               // level
  Order *tail; // pointer to the tail of the linked list of orders at this price
               // level
};
