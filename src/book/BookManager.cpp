#include "astra/book/BookManager.hpp"
#include "astra/protocol/MessageType.hpp"

BookManager::BookManager() = default;

BookManager::~BookManager() = default;

void BookManager::onMessage(const MarketDataMessage &msg) {
  OrderBook *book = nullptr;

  switch (msg.type) {
  case MessageType::AddOrder:
    book = getOrCreateOrderBook(msg.symbol_id);
    book->addOrder(msg);
    break;
  case MessageType::ModifyOrder:
    book = findOrderBook(msg.symbol_id);
    if (book == nullptr) {
      return;
    }
    book->modifyOrder(msg);
    break;
  case MessageType::DeleteOrder:
    book = findOrderBook(msg.symbol_id);
    if (book == nullptr) {
      return;
    }
    book->deleteOrder(msg);
    break;
  case MessageType::Trade:
    book = findOrderBook(msg.symbol_id);
    if (book == nullptr) {
      return;
    }
    book->trade(msg);
    break;
  default:
    return;
  }
}

const OrderBook *BookManager::getOrderBook(uint32_t symbol_id) const {
  if (symbol_id >= book_by_symbol_id_.size()) {
    return nullptr;
  }

  return book_by_symbol_id_[symbol_id];
}

OrderBook *BookManager::findOrderBook(uint32_t symbol_id) {
  if (symbol_id >= book_by_symbol_id_.size()) {
    return nullptr;
  }

  return book_by_symbol_id_[symbol_id];
}

OrderBook *BookManager::getOrCreateOrderBook(uint32_t symbol_id) {
  if (symbol_id >= book_by_symbol_id_.size()) {
    book_by_symbol_id_.resize(static_cast<size_t>(symbol_id) + 1);
  }

  OrderBook *book = book_by_symbol_id_[symbol_id];
  if (book != nullptr) {
    return book;
  }

  books_.push_back(std::make_unique<OrderBook>(symbol_id));
  book = books_.back().get();
  book_by_symbol_id_[symbol_id] = book;
  return book;
}
