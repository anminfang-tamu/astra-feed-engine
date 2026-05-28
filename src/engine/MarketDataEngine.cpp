#include "astra/engine/MarketDataEngine.hpp"
#include "astra/core/Time.hpp"
#include "astra/source/PacketView.hpp"
#include "astra/utils/DebugTrace.hpp"

MarketDataEngine::MarketDataEngine(IMarketDataSource &source,
                                   IPacketProcessor &processor,
                                   LatencyRecorder &latency_recorder,
                                   EngineConfig &config)
    : source_(source), processor_(processor),
      latency_recorder_(latency_recorder), config_(config) {}

MarketDataEngine::MarketDataEngine(IMarketDataSource &source,
                                   IPacketProcessor &processor,
                                   LatencyRecorder &latency_recorder,
                                   StageLatencyRecorder *stage_latency_recorder,
                                   EngineConfig &config)
    : source_(source), processor_(processor),
      latency_recorder_(latency_recorder),
      stage_latency_recorder_(stage_latency_recorder), config_(config) {}

bool MarketDataEngine::stop() { return running_.exchange(false); }

void MarketDataEngine::run() {
  while (running_) {
    PacketView packet;
    if (!source_.next(packet))
      continue;
    // ASTRA_TRACE("engine packet data=%p size=%zu rx_ns=%llu",
    //             static_cast<const void *>(packet.data), packet.size,
    //             static_cast<unsigned long long>(packet.receive_ts_ns));

    const bool latency_enabled = config_.enable_latency_metrics;
    const bool stage_latency_enabled =
        latency_enabled && config_.enable_stage_latency_metrics &&
        stage_latency_recorder_ != nullptr;
    const uint64_t decode_start_ns = stage_latency_enabled ? nowNs() : 0;
    const DecodeResult result = processor_.processPacket(packet);
    const uint64_t decode_done_ns = latency_enabled ? nowNs() : 0;
    // ASTRA_TRACE("engine decoded status=%d had_gap=%d",
    //             static_cast<int>(result.status), result.had_gap ? 1 : 0);

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

    if (latency_enabled) {
      if (decode_done_ns >= packet.receive_ts_ns)
        latency_recorder_.recordDuration(
            elapsedNs(packet.receive_ts_ns, decode_done_ns));
      else
        latency_recorder_.record(packet.receive_ts_ns, decode_done_ns);
      if (stage_latency_enabled) {
        if (const DecodeStageTiming *timing = processor_.lastStageTiming()) {
          stage_latency_recorder_->record(packet.receive_ts_ns, decode_start_ns,
                                          decode_done_ns, *timing);
        }
      }
    }
  }
}
