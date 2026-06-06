#include "astra/codec/IPacketProcessor.hpp"
#include "astra/engine/MarketDataEngine.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
#include "astra/metrics/StageLatencyRecorder.hpp"
#include "astra/source/IMarketDataSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

class ScriptedSource : public IMarketDataSource {
public:
  bool next(PacketView &packet) override {
    if (index_ >= packets_.size())
      return false;
    packet = packets_[index_++];
    return true;
  }

private:
  std::array<PacketView, 2> packets_{
      PacketView{nullptr, 0, 1},
      PacketView{nullptr, 0, 1},
  };
  std::size_t index_{0};
};

class ScriptedProcessor : public IPacketProcessor {
public:
  DecodeResult processPacket(const PacketView &) override {
    ++process_calls_;
    return process_calls_ == 1 ? DecodeResult{DecodeStatus::Ok, false, 40, 2}
                               : DecodeResult{DecodeStatus::EndOfStream};
  }

  const DecodeStageTiming *lastStageTiming() const noexcept override {
    ++last_stage_timing_calls_;
    return &timing_;
  }

  int processCalls() const noexcept { return process_calls_; }
  int lastStageTimingCalls() const noexcept {
    return last_stage_timing_calls_;
  }

private:
  int process_calls_{0};
  mutable int last_stage_timing_calls_{0};
  DecodeStageTiming timing_{};
};

} // namespace

TEST(MarketDataEngineTest, PacketLatencyModeDoesNotReadStageTiming) {
  ScriptedSource source;
  ScriptedProcessor processor;
  LatencyRecorder latency_recorder;
  StageLatencyRecorder stage_latency_recorder;
  EngineConfig config;
  config.enable_latency_metrics = true;
  config.enable_stage_latency_metrics = false;

  MarketDataEngine engine(source, processor, latency_recorder,
                          &stage_latency_recorder, config);
  engine.run();

  EXPECT_EQ(processor.processCalls(), 2);
  EXPECT_EQ(latency_recorder.count(), 2u);
  EXPECT_EQ(processor.lastStageTimingCalls(), 0);
}

TEST(MarketDataEngineTest, StageLatencyModeDoesNotReadStageTiming) {
  ScriptedSource source;
  ScriptedProcessor processor;
  LatencyRecorder latency_recorder;
  StageLatencyRecorder stage_latency_recorder;
  EngineConfig config;
  config.enable_latency_metrics = true;
  config.enable_stage_latency_metrics = true;

  MarketDataEngine engine(source, processor, latency_recorder,
                          &stage_latency_recorder, config);
  engine.run();

  EXPECT_EQ(processor.processCalls(), 2);
  EXPECT_EQ(latency_recorder.count(), 2u);
  EXPECT_EQ(processor.lastStageTimingCalls(), 0);
}
