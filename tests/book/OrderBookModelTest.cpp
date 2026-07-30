#include "astra/book/BookManager.hpp"
#include "astra/protocol/ItchPrice.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

enum class OpType : std::uint8_t { Add, Execute, Cancel, Delete, Replace };

struct Operation {
  OpType type;
  std::uint16_t locate;
  std::uint64_t old_ref;
  std::uint64_t new_ref;
  std::uint32_t price;
  std::uint32_t qty;
  char side;
};

struct ModelOrder {
  std::uint16_t locate;
  std::uint32_t price;
  std::uint32_t qty;
  char side;
  std::uint64_t ref;
};

struct Aggregate {
  std::uint64_t qty{0};
  std::uint32_t count{0};
};

struct ReferenceBook {
  std::map<std::uint32_t, Aggregate> bids;
  std::map<std::uint32_t, Aggregate> asks;
};

std::uint64_t nextRandom(std::uint64_t &state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

std::uint64_t newReference(std::uint64_t sequence) {
  return sequence % 7 == 0 ? (std::uint64_t{1} << 63) | sequence
                           : sequence;
}

std::uint32_t newPrice(std::uint64_t sequence, std::uint64_t &rng) {
  if (sequence % 37 == 0)
    return 0;
  if (sequence % 41 == 0)
    return astra::protocol::kMaxItchPrice4;
  return static_cast<std::uint32_t>(
      nextRandom(rng) %
      (std::uint64_t{astra::protocol::kMaxItchPrice4} + 1));
}

void removeLive(std::vector<ModelOrder> &live, std::size_t index) {
  live[index] = live.back();
  live.pop_back();
}

std::vector<Operation> makeOperations() {
  std::vector<Operation> operations;
  std::vector<ModelOrder> live;
  std::uint64_t rng = 0x6a09e667f3bcc909ULL;
  std::uint64_t next_ref = 1;

  operations.reserve(4'000);
  live.reserve(2'000);
  for (std::uint32_t step = 0; step < 3'000; ++step) {
    const std::uint64_t roll = nextRandom(rng) % 100;
    if (live.empty() || roll < 36) {
      const std::uint64_t ref = newReference(next_ref++);
      const std::uint16_t locate =
          (nextRandom(rng) & 1u) == 0 ? 1u : 65'535u;
      const char side = (nextRandom(rng) & 1u) == 0 ? 'B' : 'S';
      const std::uint32_t price = newPrice(next_ref, rng);
      const std::uint32_t qty =
          static_cast<std::uint32_t>((nextRandom(rng) % 1'000) + 1);
      operations.push_back(
          {OpType::Add, locate, ref, 0, price, qty, side});
      live.push_back({locate, price, qty, side, ref});
      continue;
    }

    const std::size_t index =
        static_cast<std::size_t>(nextRandom(rng) % live.size());
    ModelOrder &order = live[index];
    if (roll < 58) {
      const std::uint32_t qty =
          static_cast<std::uint32_t>((nextRandom(rng) % order.qty) + 1);
      operations.push_back({OpType::Execute, order.locate, order.ref, 0, 0,
                            qty, order.side});
      if (qty == order.qty)
        removeLive(live, index);
      else
        order.qty -= qty;
    } else if (roll < 72) {
      if (order.qty == 1) {
        operations.push_back({OpType::Delete, order.locate, order.ref, 0, 0,
                              0, order.side});
        removeLive(live, index);
      } else {
        const std::uint32_t qty = static_cast<std::uint32_t>(
            (nextRandom(rng) % (order.qty - 1)) + 1);
        operations.push_back({OpType::Cancel, order.locate, order.ref, 0, 0,
                              qty, order.side});
        order.qty -= qty;
      }
    } else if (roll < 84) {
      operations.push_back({OpType::Delete, order.locate, order.ref, 0, 0, 0,
                            order.side});
      removeLive(live, index);
    } else {
      const std::uint64_t replacement = newReference(next_ref++);
      const std::uint32_t price = newPrice(next_ref, rng);
      const std::uint32_t qty =
          static_cast<std::uint32_t>((nextRandom(rng) % 1'000) + 1);
      operations.push_back({OpType::Replace, order.locate, order.ref,
                            replacement, price, qty, order.side});
      order.ref = replacement;
      order.price = price;
      order.qty = qty;
    }
  }

  for (const ModelOrder &order : live) {
    operations.push_back({OpType::Delete, order.locate, order.ref, 0, 0, 0,
                          order.side});
  }
  return operations;
}

std::map<std::uint32_t, Aggregate> &levelsFor(ReferenceBook &book,
                                              char side) {
  return side == 'B' ? book.bids : book.asks;
}

void addAggregate(ReferenceBook &book, const ModelOrder &order) {
  Aggregate &level = levelsFor(book, order.side)[order.price];
  level.qty += order.qty;
  ++level.count;
}

void removeAggregate(ReferenceBook &book, const ModelOrder &order,
                     std::uint32_t qty, bool remove_order) {
  auto &levels = levelsFor(book, order.side);
  auto it = levels.find(order.price);
  ASSERT_NE(it, levels.end());
  ASSERT_GE(it->second.qty, qty);
  it->second.qty -= qty;
  if (remove_order)
    --it->second.count;
  if (it->second.count == 0) {
    EXPECT_EQ(it->second.qty, 0u);
    levels.erase(it);
  }
}

std::size_t locateIndex(std::uint16_t locate) {
  return locate == 1 ? 0 : 1;
}

void compareBook(const OrderBook &actual, const ReferenceBook &expected) {
  const TopOfBook top = actual.getTopOfBook();
  EXPECT_EQ(top.has_bid, !expected.bids.empty());
  EXPECT_EQ(top.has_ask, !expected.asks.empty());
  if (!expected.bids.empty()) {
    const auto &best = *expected.bids.rbegin();
    EXPECT_EQ(top.bid_price, best.first);
    EXPECT_EQ(top.bid_qty, best.second.qty);
  }
  if (!expected.asks.empty()) {
    const auto &best = *expected.asks.begin();
    EXPECT_EQ(top.ask_price, best.first);
    EXPECT_EQ(top.ask_qty, best.second.qty);
  }

  const BookUpdate update = actual.getBookUpdate();
  const std::size_t expected_bid_depth =
      std::min<std::size_t>(10, expected.bids.size());
  const std::size_t expected_ask_depth =
      std::min<std::size_t>(10, expected.asks.size());
  ASSERT_EQ(update.bids_depth, expected_bid_depth);
  ASSERT_EQ(update.asks_depth, expected_ask_depth);

  std::size_t rank = 0;
  for (auto it = expected.bids.rbegin();
       it != expected.bids.rend() && rank < expected_bid_depth; ++it, ++rank) {
    EXPECT_EQ(update.bids[rank].price, it->first);
    EXPECT_EQ(update.bids[rank].qty, it->second.qty);
    EXPECT_EQ(update.bids[rank].num_orders, it->second.count);
  }
  rank = 0;
  for (auto it = expected.asks.begin();
       it != expected.asks.end() && rank < expected_ask_depth; ++it, ++rank) {
    EXPECT_EQ(update.asks[rank].price, it->first);
    EXPECT_EQ(update.asks[rank].qty, it->second.qty);
    EXPECT_EQ(update.asks[rank].num_orders, it->second.count);
  }
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  return hash;
}

std::uint64_t replay(const std::vector<Operation> &operations) {
  BookManager manager(
      BookManagerConfig{{16'384, 8'192, false}, {4'096, false}});
  if (manager.getOrCreate(1) == nullptr ||
      manager.getOrCreate(65'535) == nullptr) {
    ADD_FAILURE() << "failed to prepare model-test books";
    return 0;
  }

  std::unordered_map<std::uint64_t, ModelOrder> orders;
  std::array<ReferenceBook, 2> reference;
  std::uint64_t digest = 0xcbf29ce484222325ULL;

  for (std::size_t index = 0; index < operations.size(); ++index) {
    const Operation &op = operations[index];
    astra::book::MutationResult result =
        astra::book::MutationResult::InvariantViolation;
    ReferenceBook &book = reference[locateIndex(op.locate)];

    switch (op.type) {
    case OpType::Add: {
      result = manager.addOrder(op.locate, op.old_ref, op.price, op.qty,
                                op.side);
      ModelOrder order{op.locate, op.price, op.qty, op.side, op.old_ref};
      orders.emplace(op.old_ref, order);
      addAggregate(book, order);
      break;
    }
    case OpType::Execute: {
      result = manager.executeOrder(op.locate, op.old_ref, op.qty);
      auto it = orders.find(op.old_ref);
      if (it == orders.end()) {
        ADD_FAILURE() << "oracle missing executed order";
        return 0;
      }
      const bool full = op.qty == it->second.qty;
      removeAggregate(book, it->second, op.qty, full);
      if (full)
        orders.erase(it);
      else
        it->second.qty -= op.qty;
      break;
    }
    case OpType::Cancel: {
      result = manager.cancelShares(op.locate, op.old_ref, op.qty);
      auto it = orders.find(op.old_ref);
      if (it == orders.end()) {
        ADD_FAILURE() << "oracle missing canceled order";
        return 0;
      }
      removeAggregate(book, it->second, op.qty, false);
      it->second.qty -= op.qty;
      break;
    }
    case OpType::Delete: {
      result = manager.deleteOrder(op.locate, op.old_ref);
      auto it = orders.find(op.old_ref);
      if (it == orders.end()) {
        ADD_FAILURE() << "oracle missing deleted order";
        return 0;
      }
      removeAggregate(book, it->second, it->second.qty, true);
      orders.erase(it);
      break;
    }
    case OpType::Replace: {
      result = manager.replaceOrder(op.locate, op.old_ref, op.new_ref,
                                    op.price, op.qty);
      auto it = orders.find(op.old_ref);
      if (it == orders.end()) {
        ADD_FAILURE() << "oracle missing replaced order";
        return 0;
      }
      const char side = it->second.side;
      removeAggregate(book, it->second, it->second.qty, true);
      orders.erase(it);
      ModelOrder replacement{op.locate, op.price, op.qty, side, op.new_ref};
      orders.emplace(op.new_ref, replacement);
      addAggregate(book, replacement);
      break;
    }
    }

    EXPECT_EQ(result, astra::book::MutationResult::Applied)
        << "operation index " << index;
    digest = mix(digest, static_cast<std::uint8_t>(result));
    compareBook(*manager.getOrderBook(1), reference[0]);
    compareBook(*manager.getOrderBook(65'535), reference[1]);
    const TopOfBook one = manager.getOrderBook(1)->getTopOfBook();
    const TopOfBook max = manager.getOrderBook(65'535)->getTopOfBook();
    digest = mix(digest, one.bid_price);
    digest = mix(digest, one.ask_price);
    digest = mix(digest, max.bid_qty);
    digest = mix(digest, max.ask_qty);
  }

  compareBook(*manager.getOrderBook(1), reference[0]);
  compareBook(*manager.getOrderBook(65'535), reference[1]);
  EXPECT_TRUE(orders.empty());
  EXPECT_TRUE(reference[0].bids.empty());
  EXPECT_TRUE(reference[0].asks.empty());
  EXPECT_TRUE(reference[1].bids.empty());
  EXPECT_TRUE(reference[1].asks.empty());
  return digest;
}

} // namespace

TEST(OrderBookModelTest, FixedSeedReplayMatchesOracleAndIsDeterministic) {
  const std::vector<Operation> operations = makeOperations();
  ASSERT_GT(operations.size(), 3'000u);

  const std::uint64_t first = replay(operations);
  const std::uint64_t second = replay(operations);
  EXPECT_EQ(first, second);
}
