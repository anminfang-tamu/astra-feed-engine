#include "astra/engine/MarketDataEngine.hpp"
#include "astra/core/Time.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/source/PacketView.hpp"

MarketDataEngine::MarketDataEngine(IMarketDataSource &source,
                                   BinaryDecoder &decoder,
                                   SequenceTracker &sequence_tracker,
                                   BookManager &book_manager,
                                   IPublisher &publisher,
                                   LatencyRecorder &latency_recorder,
                                   EngineConfig &config)
    : source_(source), decoder_(decoder), sequence_tracker_(sequence_tracker),
      book_manager_(book_manager), publisher_(publisher),
      latency_recorder_(latency_recorder), config_(config) {}

bool MarketDataEngine::stop() {
  bool was_running = running_.exchange(false);
  return was_running;
}

void MarketDataEngine::run() {
  while (running_) {
    PacketView packet;

    if (!source_.next(packet)) {
      continue;
    }

    MarketDataMessageView message;

    auto decode_result = decoder_.decode(packet, message);
    if (decode_result.status != DecodeStatus::Ok) {
      if (config_.stop_on_decode_error) {
        stop();
      }
      continue;
    }

    auto seq_status = sequence_tracker_.onMessage(message.seqNum());

    if (seq_status == SequenceStatus::Gap) {
      if (config_.stop_on_sequence_gap) {
        stop();
      }
      // logging
      continue;
    }

    book_manager_.onMessage(message);

    publisher_.publish();

    if (config_.enable_latency_metrics) {
      latency_recorder_.record(message.exchangeTsNs(), nowNs());
    }
  }
}