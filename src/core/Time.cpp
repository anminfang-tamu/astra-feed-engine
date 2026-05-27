#include "astra/core/Time.hpp"

#include <cstdint>
#include <ctime>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

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
