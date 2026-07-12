#include "astra/engine/MarketDataEngine.hpp"
#include "astra/core/Time.hpp"
#include "astra/source/PacketView.hpp"

namespace {

void configureTiming(IMarketDataSource &source,
                     const EngineConfig &config) {
  source.setTimestampingEnabled(config.enable_latency_metrics);
  if (config.enable_latency_metrics) {
    // Complete the one-time calibration before the engine advertises
    // readiness and before the first packet timestamp is captured.
    (void)timeCalibration();
  }
}

} // namespace

MarketDataEngine::MarketDataEngine(IMarketDataSource &source,
                                   IPacketProcessor &processor,
                                   LatencyRecorder &latency_recorder,
                                   EngineConfig &config)
    : source_(source), processor_(processor),
      latency_recorder_(latency_recorder), config_(config) {
  configureTiming(source_, config_);
}

MarketDataEngine::MarketDataEngine(IMarketDataSource &source,
                                   IPacketProcessor &processor,
                                   LatencyRecorder &latency_recorder,
                                   StageLatencyRecorder *stage_latency_recorder,
                                   EngineConfig &config)
    : source_(source), processor_(processor),
      latency_recorder_(latency_recorder), config_(config) {
  (void)stage_latency_recorder;
  configureTiming(source_, config_);
}

bool MarketDataEngine::stop() { return running_.exchange(false); }

void MarketDataEngine::run() {
  while (running_) {
    PacketView packet;
    if (!source_.next(packet))
      continue;

    const bool latency_enabled = config_.enable_latency_metrics;
    const DecodeResult result = processor_.processPacket(packet);

    if (result.status == DecodeStatus::EndOfStream) {
      stop();
      break;
    }
    if (result.status != DecodeStatus::Ok) {
      if (config_.stop_on_decode_error)
        stop();
      continue;
    }
    if (result.had_gap && config_.stop_on_sequence_gap) {
      stop();
      continue;
    }

    if (latency_enabled && result.latency_sample_count != 0) {
      latency_recorder_.recordDuration(result.latency_total_ns /
                                           result.latency_sample_count,
                                       result.latency_sample_count);
    }
  }
}
