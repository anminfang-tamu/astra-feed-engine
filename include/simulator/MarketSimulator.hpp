#pragma once

#include "astra/protocol/MarketDataMessage.hpp"
#include "astra/protocol/MessageType.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

// Simulates a single-symbol order book with realistic price, qty, side, and
// message-type distribution. Price follows an Ornstein-Uhlenbeck random walk
// so it mean-reverts to the base price instead of drifting without bound.
class MarketSimulator {
public:
  static constexpr uint32_t    kDefaultSymbolId    = 1;     // AAPL
  static constexpr uint32_t    kDefaultBaseCents   = 19500; // $195.00
  static constexpr uint32_t    kMinPriceCents      = 18000; // $180.00
  static constexpr uint32_t    kMaxPriceCents      = 21000; // $210.00
  static constexpr uint32_t    kHalfSpreadCents    = 2;
  static constexpr std::size_t kMaxActiveOrders    = 2000;

  explicit MarketSimulator(uint32_t symbol_id       = kDefaultSymbolId,
                           uint32_t base_price_cents = kDefaultBaseCents)
      : symbol_id_(symbol_id)
      , base_price_(base_price_cents)
      , mid_price_(base_price_cents)
      , rng_(std::random_device{}())
      , drift_dist_(0.0, 0.5)            // 0.5 cent std dev per step
      , qty_dist_(std::log(50.0), 0.8)   // log-normal: median ~50 shares
      , type_dist_({45, 35, 12, 8})      // Add : Delete : Modify : Trade
      , side_dist_(0, 1)
  {
    active_orders_.reserve(kMaxActiveOrders + 256);
    order_ids_.reserve(kMaxActiveOrders + 256);
  }

  // Populates msg with the next simulated market data message.
  void next(MarketDataMessage &msg) {
    stepPrice();

    msg.seq_num       = seq_num_++;
    msg.symbol_id     = symbol_id_;
    msg.exhange_ts_ns = nowNs();

    // Cap book growth; force AddOrder when empty, DeleteOrder when full.
    int type_idx = type_dist_(rng_);
    if (order_ids_.empty()) {
      type_idx = 0;
    } else if (order_ids_.size() >= kMaxActiveOrders && type_idx == 0) {
      type_idx = 1;
    }

    switch (type_idx) {
    case 0: emitAdd(msg);    break;
    case 1: emitDelete(msg); break;
    case 2: emitModify(msg); break;
    case 3: emitTrade(msg);  break;
    }
  }

  uint32_t    midPrice()   const { return mid_price_;       }
  uint64_t    seqNum()     const { return seq_num_;          }
  std::size_t bookDepth()  const { return order_ids_.size(); }

private:
  struct OrderEntry {
    uint32_t  price;
    OrderSide side;
  };

  // Ornstein-Uhlenbeck step: pulls price back toward base while adding noise.
  void stepPrice() {
    const double mean_reversion = -0.001 *
        static_cast<double>(static_cast<int64_t>(mid_price_) -
                            static_cast<int64_t>(base_price_));
    const int64_t delta = static_cast<int64_t>(mean_reversion + drift_dist_(rng_));
    const int64_t next  = static_cast<int64_t>(mid_price_) + delta;
    mid_price_ = static_cast<uint32_t>(
        std::clamp(next,
                   static_cast<int64_t>(kMinPriceCents),
                   static_cast<int64_t>(kMaxPriceCents)));
  }

  uint32_t randomQty() {
    const int q = static_cast<int>(qty_dist_(rng_));
    return static_cast<uint32_t>(std::clamp(q, 1, 10000));
  }

  std::size_t randomIdx() {
    return rng_() % order_ids_.size();
  }

  static uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  void emitAdd(MarketDataMessage &msg) {
    const OrderSide side  = side_dist_(rng_) ? OrderSide::Buy : OrderSide::Sell;
    const uint32_t  price = (side == OrderSide::Buy)
                                ? mid_price_ - kHalfSpreadCents
                                : mid_price_ + kHalfSpreadCents;
    const uint64_t  oid   = next_order_id_++;

    msg.order_id = oid;
    msg.price    = price;
    msg.qty      = randomQty();
    msg.type     = MessageType::AddOrder;
    msg.side     = side;

    active_orders_[oid] = {price, side};
    order_ids_.push_back(oid);
  }

  void emitDelete(MarketDataMessage &msg) {
    const std::size_t idx = randomIdx();
    const uint64_t    oid = order_ids_[idx];
    const OrderEntry &e   = active_orders_.at(oid);

    msg.order_id = oid;
    msg.price    = e.price;
    msg.qty      = 0;
    msg.type     = MessageType::DeleteOrder;
    msg.side     = e.side;

    active_orders_.erase(oid);
    order_ids_[idx] = order_ids_.back();
    order_ids_.pop_back();
  }

  void emitModify(MarketDataMessage &msg) {
    const std::size_t idx = randomIdx();
    const uint64_t    oid = order_ids_[idx];
    const OrderEntry &e   = active_orders_.at(oid);

    msg.order_id = oid;
    msg.price    = e.price;
    msg.qty      = randomQty();
    msg.type     = MessageType::ModifyOrder;
    msg.side     = e.side;
  }

  void emitTrade(MarketDataMessage &msg) {
    const std::size_t idx = randomIdx();
    const uint64_t    oid = order_ids_[idx];
    const OrderEntry &e   = active_orders_.at(oid);

    msg.order_id = oid;
    msg.price    = e.price;
    msg.qty      = randomQty();
    msg.type     = MessageType::Trade;
    msg.side     = e.side;

    // fully filled — remove from book
    active_orders_.erase(oid);
    order_ids_[idx] = order_ids_.back();
    order_ids_.pop_back();
  }

  uint32_t symbol_id_;
  uint32_t base_price_;
  uint32_t mid_price_;
  uint64_t next_order_id_{1};
  uint64_t seq_num_{1};

  std::mt19937                           rng_;
  std::normal_distribution<double>       drift_dist_;
  std::lognormal_distribution<double>    qty_dist_;
  std::discrete_distribution<int>        type_dist_;
  std::uniform_int_distribution<int>     side_dist_;

  std::unordered_map<uint64_t, OrderEntry> active_orders_;
  std::vector<uint64_t>                    order_ids_;
};
