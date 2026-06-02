#pragma once

#include <cstdint>

struct TimeCalibration {
  uint64_t now_ns_overhead_ns{0};
  uint64_t rdtsc_overhead_ticks{0};
  uint64_t rdtsc_ticks_per_second{0};
};

uint64_t nowNs();
uint64_t rdtsc();
const TimeCalibration &timeCalibration();
uint64_t elapsedNs(uint64_t start_ns, uint64_t end_ns);
uint64_t elapsedRdtscNs(uint64_t start_ticks, uint64_t end_ticks);
