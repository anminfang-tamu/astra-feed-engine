#include "astra/core/Time.hpp"

#include <chrono>
#include <cstdint>
#include <sys/types.h>

uint64_t nowNs() {
  using clock = std::chrono::steady_clock;

  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}