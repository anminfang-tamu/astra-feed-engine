#pragma once

#include <cstdint>

#include "Order.hpp"

struct PriceLevel {    // 12 bytes total
  uint32_t price;      // 4 bytes, price level
  uint32_t qty;        // 4 bytes, total quantity at this price level
  uint32_t num_orders; // 4 bytes, number of orders at this price level

  Order *head; // pointer to the head of the linked list of orders at this price
               // level
  Order *tail; // pointer to the tail of the linked list of orders at this price
               // level
};