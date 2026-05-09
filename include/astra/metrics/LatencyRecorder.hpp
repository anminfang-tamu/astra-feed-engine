#pragma once

#include <cstdint>

class LatencyRecorder {
public:
  void record(std::uint64_t start_ns, std::uint64_t end_ns);
};