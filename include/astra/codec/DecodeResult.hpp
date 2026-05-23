#pragma once

#include <cstddef>

enum class DecodeStatus {
  Ok,
  PacketTooSmall,
  InvalidMessageType,
  InvalidSize,
  UnSupportedVersion,
  InvalidOrderSide,
  Heartbeat,
  EndOfStream,
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::Ok};
  bool         had_gap{false};
};