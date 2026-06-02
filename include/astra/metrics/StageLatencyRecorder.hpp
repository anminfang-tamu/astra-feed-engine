#pragma once

#include "astra/metrics/DecodeStageTiming.hpp"
#include "astra/metrics/LatencyRecorder.hpp"

#include <cstdint>
#include <iosfwd>

class StageLatencyRecorder {
public:
  void record(std::uint64_t recv_done_ns, std::uint64_t decode_start_ns,
              std::uint64_t done_ns, const DecodeStageTiming &timing);
  void reset();

  void report() const;
  void report(std::ostream &out) const;

private:
  static void recordDuration(LatencyRecorder &recorder,
                             std::uint64_t duration_ns);
  static void reportLine(std::ostream &out, const char *name,
                         const LatencyRecorder &recorder);

  LatencyRecorder total_;
  LatencyRecorder recv_to_decode_start_;
  LatencyRecorder mold_parse_;
  LatencyRecorder itch_parse_state_;
  LatencyRecorder book_;
  LatencyRecorder engine_other_;

  std::uint64_t packets_{0};
  std::uint64_t messages_{0};
  std::uint64_t book_messages_{0};
  std::uint64_t skipped_messages_{0};
  std::uint64_t sequenced_packets_{0};
  std::uint64_t gap_packets_{0};
  std::uint64_t gap_buffered_packets_{0};
  std::uint64_t gap_buffer_insert_failed_packets_{0};
  std::uint64_t stale_gap_dropped_packets_{0};
  std::uint64_t old_packets_{0};
  std::uint64_t itch_parse_state_total_ns_{0};
  std::uint64_t book_total_ns_{0};
};
