#pragma once

#include <cstddef>

enum class DecodeStatus {
  Ok,
  PacketTooSmall,
  InvalidMessageType,
  InvalidSize,
  UnSupportedVersion
};

struct DecodeResult {
  DecodeStatus status;
};