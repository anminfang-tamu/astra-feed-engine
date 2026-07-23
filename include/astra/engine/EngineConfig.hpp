#pragma once

struct EngineConfig {
  bool enable_latency_metrics{true};
  bool enable_stage_latency_metrics{false};
  // Sticky terminal decoder failures always stop. This flag additionally
  // stops on non-terminal malformed datagrams such as PacketTooSmall.
  bool stop_on_decode_error{false};
  bool stop_on_sequence_gap{false};
};
