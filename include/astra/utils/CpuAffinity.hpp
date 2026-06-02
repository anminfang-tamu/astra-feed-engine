#pragma once

#include <cstdint>

namespace astra::utils {

enum class CpuAffinityStatus : std::uint8_t {
  Applied,
  InvalidCpu,
  SystemError,
  Unsupported
};

struct CpuAffinityResult {
  CpuAffinityStatus status{CpuAffinityStatus::Unsupported};
  int cpu_id{-1};
  int error_code{0};
  const char *message{"unsupported platform"};

  [[nodiscard]] explicit operator bool() const {
    return status == CpuAffinityStatus::Applied;
  }
};

[[nodiscard]] unsigned int availableCpuCount() noexcept;
[[nodiscard]] CpuAffinityResult pinCurrentThreadToCpu(int cpu_id) noexcept;
[[nodiscard]] CpuAffinityResult clearCurrentThreadAffinity() noexcept;
[[nodiscard]] const char *
cpuAffinityStatusName(CpuAffinityStatus status) noexcept;

} // namespace astra::utils
