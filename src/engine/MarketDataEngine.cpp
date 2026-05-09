#include "astra/engine/MarketDataEngine.hpp"
#include "astra/core/Time.hpp"
#include "astra/protocol/MarketDataMessage.hpp"
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

void MarketDataEngine::run() {
  while (true) {
    PacketView packet;

    if (!source_.next(packet)) {
      continue;
    }

    MarketDataMessage message;

    auto decode_result = decoder_.decode(packet.data, packet.size, message);
    if (decode_result.status != DecodeStatus::Ok) {
      if (config_.stop_on_decode_error) {
        stop();
      }
      continue;
    }

    if (!sequence_tracker_.onMessage(message.seq_num)) {
      if (config_.stop_on_sequence_gap) {
        stop();
      }
      // logging
      continue;
    }

    book_manager_.onMessage(message);

    publisher_.publish();

    if (config_.enable_latency_metrics) {
      latency_recorder_.record(packet.receive_ts_ns, nowNs());
    }
  }
}