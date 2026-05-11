#pragma once

#include "astra/book/BookManager.hpp"
#include "astra/codec/BinaryDecoder.hpp"
#include "astra/engine/EngineConfig.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
#include "astra/publish/IPublisher.hpp"
#include "astra/sequencing/SequenceTracker.hpp"
#include "astra/source/IMarketDataSource.hpp"

#include <atomic>

class MarketDataEngine {
public:
  MarketDataEngine(IMarketDataSource &source, BinaryDecoder &decoder,
                   SequenceTracker &sequence_tracker, BookManager &book_manager,
                   IPublisher &publisher, LatencyRecorder &latency_recorder,
                   EngineConfig &config);

  MarketDataEngine(const MarketDataEngine &) = delete;
  MarketDataEngine &operator=(const MarketDataEngine &) = delete;

  MarketDataEngine(MarketDataEngine &&) = delete;
  MarketDataEngine &operator=(MarketDataEngine &&) = delete;

  void run();
  bool isRunning() const { return running_; }
  bool stop();

private:
  IMarketDataSource &source_;
  BinaryDecoder &decoder_;
  SequenceTracker &sequence_tracker_;
  BookManager &book_manager_;
  IPublisher &publisher_;
  LatencyRecorder &latency_recorder_;
  EngineConfig &config_;

  std::atomic<bool> running_{true};
};
