#pragma once

#include <string>

class BookManager {
public:
  BookManager();
  ~BookManager();

  void onMessage();
  const OrderBook *getOrderBook(const std::string &symbol) const;

private:
  void loadBooks();
  void saveBooks();
}