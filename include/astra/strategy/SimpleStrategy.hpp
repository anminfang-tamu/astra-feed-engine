#include "astra/strategy/IStrategy.hpp"

class SimpleStrategy : public IStrategy {
public:
  void onBookUpdate(const BookUpdate &update) override {}
};