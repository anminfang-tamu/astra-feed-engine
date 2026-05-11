#pragma once

#include <cstdint>

// store latencies in a vector and sort at the end.
// later, use a fixed histogram.
class LatencyRecorder {
public:
  void record(std::uint64_t start_ns, std::uint64_t end_ns);
  void report() const;
};