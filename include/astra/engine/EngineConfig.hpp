#pragma once

#include <cstddef>

struct EngineConfig {
  bool enable_latency_metrics{true};
  bool stop_on_decode_error{false};
  bool stop_on_sequence_gap{false};
  size_t publish_batch_size{1};
};