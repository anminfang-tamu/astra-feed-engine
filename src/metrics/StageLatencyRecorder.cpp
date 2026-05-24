#include "astra/metrics/StageLatencyRecorder.hpp"

#include <iomanip>
#include <iostream>

void StageLatencyRecorder::record(std::uint64_t recv_done_ns,
                                  std::uint64_t decode_start_ns,
                                  std::uint64_t done_ns,
                                  const DecodeStageTiming &timing) {
  ++packets_;
  messages_ += timing.messages;
  book_messages_ += timing.book_messages;
  skipped_messages_ += timing.skipped_messages;
  sequenced_packets_ += timing.sequenced_packets;
  gap_packets_ += timing.gap_packets;
  gap_buffered_packets_ += timing.gap_buffered_packets;
  gap_buffer_insert_failed_packets_ +=
      timing.gap_buffer_insert_failed_packets;
  stale_gap_dropped_packets_ += timing.stale_gap_dropped_packets;
  old_packets_ += timing.old_packets;

  const std::uint64_t total_ns =
      done_ns >= recv_done_ns ? done_ns - recv_done_ns : 0;
  const std::uint64_t recv_to_decode_ns =
      decode_start_ns >= recv_done_ns ? decode_start_ns - recv_done_ns : 0;
  const std::uint64_t itch_parse_state_ns =
      timing.itch_total_ns >= timing.book_ns
          ? timing.itch_total_ns - timing.book_ns
          : 0;
  itch_parse_state_total_ns_ += itch_parse_state_ns;
  book_total_ns_ += timing.book_ns;

  recordDuration(total_, total_ns);
  recordDuration(recv_to_decode_start_, recv_to_decode_ns);
  recordDuration(mold_parse_, timing.mold_parse_ns);
  recordDuration(itch_parse_state_, itch_parse_state_ns);
  recordDuration(book_, timing.book_ns);

  const std::uint64_t accounted_ns =
      recv_to_decode_ns + timing.mold_parse_ns + itch_parse_state_ns +
      timing.book_ns;
  recordDuration(engine_other_,
                 total_ns > accounted_ns ? total_ns - accounted_ns : 0);
}

void StageLatencyRecorder::reset() {
  total_.reset();
  recv_to_decode_start_.reset();
  mold_parse_.reset();
  itch_parse_state_.reset();
  book_.reset();
  engine_other_.reset();
  packets_ = 0;
  messages_ = 0;
  book_messages_ = 0;
  skipped_messages_ = 0;
  sequenced_packets_ = 0;
  gap_packets_ = 0;
  gap_buffered_packets_ = 0;
  gap_buffer_insert_failed_packets_ = 0;
  stale_gap_dropped_packets_ = 0;
  old_packets_ = 0;
  itch_parse_state_total_ns_ = 0;
  book_total_ns_ = 0;
}

void StageLatencyRecorder::report() const { report(std::cout); }

void StageLatencyRecorder::report(std::ostream &out) const {
  out << "stage_latency packets=" << packets_ << " messages=" << messages_
      << " book_messages=" << book_messages_
      << " skipped_messages=" << skipped_messages_;
  if (packets_ != 0) {
    const long double msgs_per_packet =
        static_cast<long double>(messages_) / static_cast<long double>(packets_);
    out << " mean_msgs_per_packet=" << std::fixed << std::setprecision(2)
        << static_cast<double>(msgs_per_packet) << std::defaultfloat;
  }
  out << '\n';
  out << "stage_sequence sequenced_packets=" << sequenced_packets_
      << " gap_packets=" << gap_packets_
      << " gap_buffered_packets=" << gap_buffered_packets_
      << " gap_buffer_insert_failed_packets="
      << gap_buffer_insert_failed_packets_
      << " stale_gap_dropped_packets=" << stale_gap_dropped_packets_
      << " old_packets=" << old_packets_ << '\n';
  if (messages_ != 0 || book_messages_ != 0) {
    out << "stage_per_message";
    if (messages_ != 0) {
      const long double itch_parse_state_mean =
          static_cast<long double>(itch_parse_state_total_ns_) /
          static_cast<long double>(messages_);
      out << " itch_parse_state_mean_ns=" << std::fixed
          << std::setprecision(2)
          << static_cast<double>(itch_parse_state_mean) << std::defaultfloat;
    }
    if (book_messages_ != 0) {
      const long double book_mean =
          static_cast<long double>(book_total_ns_) /
          static_cast<long double>(book_messages_);
      out << " book_update_mean_ns=" << std::fixed << std::setprecision(2)
          << static_cast<double>(book_mean) << std::defaultfloat;
    }
    out << '\n';
  }

  reportLine(out, "total_recv_done_to_decode_done", total_);
  reportLine(out, "recv_done_to_decode_start", recv_to_decode_start_);
  reportLine(out, "mold_header_framing", mold_parse_);
  reportLine(out, "itch_parse_state", itch_parse_state_);
  reportLine(out, "book_update", book_);
  reportLine(out, "engine_other", engine_other_);
}

void StageLatencyRecorder::recordDuration(LatencyRecorder &recorder,
                                          std::uint64_t duration_ns) {
  recorder.recordDuration(duration_ns);
}

void StageLatencyRecorder::reportLine(std::ostream &out, const char *name,
                                      const LatencyRecorder &recorder) {
  const LatencyRecorder::Snapshot stats = recorder.snapshot();
  out << "stage " << name << " count=" << stats.count
      << " invalid=" << stats.invalid_samples;
  if (stats.count == 0) {
    out << '\n';
    return;
  }

  out << " min_ns=" << stats.min_ns << " max_ns=" << stats.max_ns
      << " mean_ns=" << std::fixed << std::setprecision(2)
      << static_cast<double>(stats.mean_ns) << std::defaultfloat
      << " p50_ns=" << stats.p50_ns << " p90_ns=" << stats.p90_ns
      << " p99_ns=" << stats.p99_ns << " p99.9_ns=" << stats.p999_ns
      << " p99.99_ns=" << stats.p9999_ns << '\n';
}
