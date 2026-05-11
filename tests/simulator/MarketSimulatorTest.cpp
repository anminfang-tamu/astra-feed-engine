#include "simulator/MarketSimulator.hpp"

#include <gtest/gtest.h>

TEST(MarketSimulatorTest, DefaultConstructionInitialState) {
  MarketSimulator sim;

  EXPECT_EQ(sim.midPrice(), MarketSimulator::kDefaultBaseCents);
  EXPECT_EQ(sim.seqNum(), 1u);
  EXPECT_EQ(sim.bookDepth(), 0u);
}

TEST(MarketSimulatorTest, CustomConstructionUsesGivenValues) {
  constexpr uint32_t symbol = 99;
  constexpr uint32_t base = 50000;
  MarketSimulator sim(symbol, base);

  EXPECT_EQ(sim.midPrice(), base);

  MarketDataMessage msg{};
  sim.next(msg);

  EXPECT_EQ(msg.symbol_id, symbol);
}

TEST(MarketSimulatorTest, SeqNumIncrementsMonotonically) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (uint64_t expected = 1; expected <= 50; ++expected) {
    sim.next(msg);
    EXPECT_EQ(msg.seq_num, expected);
  }

  EXPECT_EQ(sim.seqNum(), 51u);
}

TEST(MarketSimulatorTest, SymbolIdMatchesConstructorArg) {
  constexpr uint32_t symbol = 7;
  MarketSimulator sim(symbol);
  MarketDataMessage msg{};

  for (int i = 0; i < 20; ++i) {
    sim.next(msg);
    EXPECT_EQ(msg.symbol_id, symbol);
  }
}

TEST(MarketSimulatorTest, FirstMessageOnEmptyBookIsAddOrder) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  sim.next(msg);

  EXPECT_EQ(msg.type, MessageType::AddOrder);
  EXPECT_EQ(sim.bookDepth(), 1u);
}

TEST(MarketSimulatorTest, BookDepthIncreasesOnAddOrder) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  // Drive the first N messages; count adds
  std::size_t expected_depth = 0;
  for (int i = 0; i < 200; ++i) {
    sim.next(msg);
    if (msg.type == MessageType::AddOrder) {
      ++expected_depth;
    } else if (msg.type == MessageType::DeleteOrder ||
               msg.type == MessageType::Trade) {
      --expected_depth;
    }
    EXPECT_EQ(sim.bookDepth(), expected_depth);
  }
}

TEST(MarketSimulatorTest, DeleteOrderHasZeroQty) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  // Seed the book first
  for (int i = 0; i < 20; ++i) {
    sim.next(msg);
  }

  for (int i = 0; i < 200; ++i) {
    sim.next(msg);
    if (msg.type == MessageType::DeleteOrder) {
      EXPECT_EQ(msg.qty, 0u);
    }
  }
}

TEST(MarketSimulatorTest, ModifyOrderDoesNotChangeBookDepth) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (int i = 0; i < 500; ++i) {
    const std::size_t before = sim.bookDepth();
    sim.next(msg);
    if (msg.type == MessageType::ModifyOrder) {
      EXPECT_EQ(sim.bookDepth(), before);
    }
  }
}

TEST(MarketSimulatorTest, MidPriceStaysWithinBounds) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (int i = 0; i < 5000; ++i) {
    sim.next(msg);
    EXPECT_GE(sim.midPrice(), MarketSimulator::kMinPriceCents);
    EXPECT_LE(sim.midPrice(), MarketSimulator::kMaxPriceCents);
  }
}

TEST(MarketSimulatorTest, AddOrderPriceReflectsSpread) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  int checked = 0;
  for (int i = 0; i < 500 && checked < 20; ++i) {
    sim.next(msg);
    if (msg.type != MessageType::AddOrder)
      continue;

    // stepPrice() runs inside next() before emitAdd(), so midPrice() is
    // current.
    if (msg.side == OrderSide::Buy) {
      EXPECT_EQ(msg.price, sim.midPrice() - MarketSimulator::kHalfSpreadCents);
    } else {
      EXPECT_EQ(msg.price, sim.midPrice() + MarketSimulator::kHalfSpreadCents);
    }
    ++checked;
  }

  EXPECT_GE(checked, 1) << "No AddOrder messages emitted in 500 steps";
}

TEST(MarketSimulatorTest, QtyIsPositiveForNonDeleteMessages) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (int i = 0; i < 500; ++i) {
    sim.next(msg);
    if (msg.type != MessageType::DeleteOrder) {
      EXPECT_GT(msg.qty, 0u);
    }
  }
}

TEST(MarketSimulatorTest, TimestampIsNonZero) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  sim.next(msg);

  EXPECT_GT(msg.exhange_ts_ns, 0u);
}

TEST(MarketSimulatorTest, BookDepthNeverExceedsMaxActiveOrders) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (int i = 0; i < 10000; ++i) {
    sim.next(msg);
    EXPECT_LE(sim.bookDepth(), MarketSimulator::kMaxActiveOrders);
  }
}

TEST(MarketSimulatorTest, SideIsAlwaysBuyOrSell) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (int i = 0; i < 500; ++i) {
    sim.next(msg);
    EXPECT_TRUE(msg.side == OrderSide::Buy || msg.side == OrderSide::Sell);
  }
}

TEST(MarketSimulatorTest, MessageTypeIsAlwaysValid) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  for (int i = 0; i < 500; ++i) {
    sim.next(msg);
    EXPECT_TRUE(msg.type == MessageType::AddOrder ||
                msg.type == MessageType::DeleteOrder ||
                msg.type == MessageType::ModifyOrder ||
                msg.type == MessageType::Trade);
  }
}

TEST(MarketSimulatorTest, StressRunDoesNotCrash) {
  MarketSimulator sim;
  MarketDataMessage msg{};

  ASSERT_NO_THROW({
    for (int i = 0; i < 50000; ++i) {
      sim.next(msg);
    }
  });
}
