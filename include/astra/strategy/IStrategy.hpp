#pragma once

#include "astra/book/BookUpdate.hpp"

class IStrategy {
public:
  virtual ~IStrategy() = default;

  virtual void onBookUpdate(const BookUpdate &update) = 0;
};