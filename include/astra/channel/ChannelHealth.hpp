#pragma once

enum class ChannelHealth {
  Good = 1,
  GapDetected = 2,
  Recovering = 3,
  Stale = 4,
  Invalid = 5,
};