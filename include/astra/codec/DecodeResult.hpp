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

constexpr const char *decodeStatusName(DecodeStatus status) noexcept {
  switch (status) {
  case DecodeStatus::Ok:
    return "Ok";
  case DecodeStatus::PacketTooSmall:
    return "PacketTooSmall";
  case DecodeStatus::InvalidMessageType:
    return "InvalidMessageType";
  case DecodeStatus::InvalidSize:
    return "InvalidSize";
  case DecodeStatus::UnSupportedVersion:
    return "UnSupportedVersion";
  case DecodeStatus::InvalidOrderSide:
    return "InvalidOrderSide";
  case DecodeStatus::Heartbeat:
    return "Heartbeat";
  case DecodeStatus::EndOfStream:
    return "EndOfStream";
  case DecodeStatus::InvalidSequence:
    return "InvalidSequence";
  case DecodeStatus::InvalidSession:
    return "InvalidSession";
  case DecodeStatus::InvalidItchMessage:
    return "InvalidItchMessage";
  case DecodeStatus::IncompleteSession:
    return "IncompleteSession";
  }
  return "Unknown";
}

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
