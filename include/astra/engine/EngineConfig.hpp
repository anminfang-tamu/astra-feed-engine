#pragma once

struct EngineConfig {
  bool enable_latency_metrics{true};
  bool stop_on_decode_error{false};
  bool stop_on_sequence_gap{false};
};
