#include "astra/core/Time.hpp"

#include <cstdint>
#include <ctime>
#include <limits>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace {

constexpr int kCalibrationSamples = 10000;
constexpr long double kNsPerSecond = 1'000'000'000.0L;

uint64_t saturatedNsFromTicks(uint64_t ticks,
                              uint64_t ticks_per_second) noexcept {
  if (ticks_per_second == 0)
    return ticks;

  const long double ns =
      (static_cast<long double>(ticks) * kNsPerSecond) /
      static_cast<long double>(ticks_per_second);
  if (ns >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
    return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(ns + 0.5L);
}

uint64_t measureNowNsOverhead() noexcept {
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < kCalibrationSamples; ++i) {
    const uint64_t start = nowNs();
    const uint64_t end = nowNs();
    if (end >= start && end - start < best)
      best = end - start;
  }
  return best == std::numeric_limits<uint64_t>::max() ? 0 : best;
}

uint64_t measureRdtscOverhead() noexcept {
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < kCalibrationSamples; ++i) {
    const uint64_t start = rdtsc();
    const uint64_t end = rdtsc();
    if (end >= start && end - start < best)
      best = end - start;
  }
  return best == std::numeric_limits<uint64_t>::max() ? 0 : best;
}

uint64_t measureRdtscFrequency() noexcept {
  timespec sleep_time{0, 10'000'000}; // 10 ms keeps startup cost small.

  const uint64_t ns_start = nowNs();
  const uint64_t ticks_start = rdtsc();
  nanosleep(&sleep_time, nullptr);
  const uint64_t ticks_end = rdtsc();
  const uint64_t ns_end = nowNs();

  if (ticks_end <= ticks_start || ns_end <= ns_start)
    return 0;

  const long double ticks =
      static_cast<long double>(ticks_end - ticks_start);
  const long double ns = static_cast<long double>(ns_end - ns_start);
  return static_cast<uint64_t>((ticks * kNsPerSecond / ns) + 0.5L);
}

TimeCalibration calibrateTime() noexcept {
  TimeCalibration calibration{};
  calibration.now_ns_overhead_ns = measureNowNsOverhead();
  calibration.rdtsc_overhead_ticks = measureRdtscOverhead();
  calibration.rdtsc_ticks_per_second = measureRdtscFrequency();
  return calibration;
}

} // namespace

uint64_t nowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
  unsigned aux;
  return __rdtscp(&aux);
#elif defined(__aarch64__)
  uint64_t ticks;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#else
  return nowNs();
#endif
}

const TimeCalibration &timeCalibration() {
  static const TimeCalibration calibration = calibrateTime();
  return calibration;
}

uint64_t elapsedNs(uint64_t start_ns, uint64_t end_ns) {
  if (end_ns < start_ns)
    return 0;

  const uint64_t elapsed = end_ns - start_ns;
  const uint64_t overhead = timeCalibration().now_ns_overhead_ns;
  return elapsed > overhead ? elapsed - overhead : 0;
}

uint64_t elapsedRdtscNs(uint64_t start_ticks, uint64_t end_ticks) {
  if (end_ticks < start_ticks)
    return 0;

  const TimeCalibration &calibration = timeCalibration();
  const uint64_t elapsed_ticks = end_ticks - start_ticks;
  const uint64_t adjusted_ticks =
      elapsed_ticks > calibration.rdtsc_overhead_ticks
          ? elapsed_ticks - calibration.rdtsc_overhead_ticks
          : 0;
  return saturatedNsFromTicks(adjusted_ticks,
                              calibration.rdtsc_ticks_per_second);
}
