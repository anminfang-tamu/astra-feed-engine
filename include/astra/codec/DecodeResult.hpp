#pragma once

#include <cstddef>
#include <cstdint>

enum class DecodeStatus {
  Ok,
  PacketTooSmall,
  InvalidMessageType,
  InvalidSize,
  UnSupportedVersion,
  InvalidOrderSide,
  Heartbeat,
  EndOfStream,
  InvalidSequence,
  InvalidItchMessage,
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::Ok};
  bool had_gap{false};
  std::uint64_t latency_total_ns{0};
  std::uint64_t latency_sample_count{0};
};
