#pragma once

#include <cstddef>

enum class DecodeStatus {
  Ok,
  PacketTooSmall,
  InvalidMessageType,
  InvalidSize,
  UnSupportedVersion,
  InvalidOrderSide
};

struct DecodeResult {
  DecodeStatus status;
};