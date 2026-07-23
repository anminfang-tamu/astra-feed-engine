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
  InvalidSession,
  InvalidItchMessage,
  IncompleteSession,
};

// These failures make MoldUdpDecoder sticky-invalid because continuing would
// apply later sequence numbers to state whose continuity or wire validity is
// no longer trustworthy. Production must stop even when best-effort handling
// is enabled for transient datagram errors.
constexpr bool isTerminalDecodeFailure(DecodeStatus status) noexcept {
  switch (status) {
  case DecodeStatus::InvalidSize:
  case DecodeStatus::InvalidSequence:
  case DecodeStatus::InvalidSession:
  case DecodeStatus::InvalidItchMessage:
  case DecodeStatus::IncompleteSession:
    return true;
  default:
    return false;
  }
}

struct DecodeResult {
  DecodeStatus status{DecodeStatus::Ok};
  bool had_gap{false};
  std::uint64_t latency_total_ns{0};
  std::uint64_t latency_sample_count{0};
};
