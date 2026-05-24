#pragma once

#include "astra/codec/IPacketProcessor.hpp"
#include "astra/engine/EngineConfig.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
#include "astra/metrics/StageLatencyRecorder.hpp"
#include "astra/source/IMarketDataSource.hpp"

#include <atomic>

class MarketDataEngine {
public:
  MarketDataEngine(IMarketDataSource &source, IPacketProcessor &processor,
                   LatencyRecorder &latency_recorder, EngineConfig &config);
  MarketDataEngine(IMarketDataSource &source, IPacketProcessor &processor,
                   LatencyRecorder &latency_recorder,
                   StageLatencyRecorder *stage_latency_recorder,
                   EngineConfig &config);

  MarketDataEngine(const MarketDataEngine &)            = delete;
  MarketDataEngine &operator=(const MarketDataEngine &) = delete;
  MarketDataEngine(MarketDataEngine &&)                 = delete;
  MarketDataEngine &operator=(MarketDataEngine &&)      = delete;

  void run();
  bool isRunning() const { return running_; }
  bool stop();

private:
  IMarketDataSource &source_;
  IPacketProcessor  &processor_;
  LatencyRecorder   &latency_recorder_;
  StageLatencyRecorder *stage_latency_recorder_{nullptr};
  EngineConfig      &config_;

  std::atomic<bool> running_{true};
};
