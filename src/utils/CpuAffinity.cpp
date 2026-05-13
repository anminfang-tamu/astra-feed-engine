#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "astra/utils/CpuAffinity.hpp"

#include <cerrno>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

namespace astra::utils {

unsigned int availableCpuCount() noexcept {
  const unsigned int count = std::thread::hardware_concurrency();
  return count == 0 ? 1 : count;
}

CpuAffinityResult pinCurrentThreadToCpu(int cpu_id) noexcept {
  if (cpu_id < 0) {
    return {CpuAffinityStatus::InvalidCpu, cpu_id, EINVAL,
            "cpu_id must be non-negative"};
  }

  if (static_cast<unsigned int>(cpu_id) >= availableCpuCount()) {
    return {CpuAffinityStatus::InvalidCpu, cpu_id, EINVAL,
            "cpu_id is outside the visible CPU range"};
  }

#if defined(__linux__)
  if (cpu_id >= CPU_SETSIZE) {
    return {CpuAffinityStatus::InvalidCpu, cpu_id, EINVAL,
            "cpu_id exceeds CPU_SETSIZE"};
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);

  const int rc =
      pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (rc != 0) {
    return {CpuAffinityStatus::SystemError, cpu_id, rc,
            "pthread_setaffinity_np failed"};
  }

  return {CpuAffinityStatus::Applied, cpu_id, 0,
          "current thread pinned to requested CPU"};
#elif defined(__APPLE__)
  thread_affinity_policy_data_t policy = {static_cast<integer_t>(cpu_id + 1)};
  const thread_t thread = mach_thread_self();
  const kern_return_t rc =
      thread_policy_set(thread, THREAD_AFFINITY_POLICY,
                        reinterpret_cast<thread_policy_t>(&policy),
                        THREAD_AFFINITY_POLICY_COUNT);
  mach_port_deallocate(mach_task_self(), thread);
  if (rc != KERN_SUCCESS) {
    return {CpuAffinityStatus::SystemError, cpu_id, static_cast<int>(rc),
            "thread_policy_set failed"};
  }

  return {CpuAffinityStatus::Applied, cpu_id, 0,
          "macOS affinity tag applied; exact CPU pinning is not exposed"};
#else
  return {CpuAffinityStatus::Unsupported, cpu_id, ENOTSUP,
          "CPU affinity is unsupported on this platform"};
#endif
}

CpuAffinityResult clearCurrentThreadAffinity() noexcept {
#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  const unsigned int cpu_count = availableCpuCount();
  for (unsigned int cpu = 0; cpu < cpu_count && cpu < CPU_SETSIZE; ++cpu) {
    CPU_SET(cpu, &cpuset);
  }

  const int rc =
      pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (rc != 0) {
    return {CpuAffinityStatus::SystemError, -1, rc,
            "pthread_setaffinity_np failed"};
  }

  return {CpuAffinityStatus::Applied, -1, 0,
          "current thread affinity reset to all known CPUs"};
#elif defined(__APPLE__)
  thread_affinity_policy_data_t policy = {THREAD_AFFINITY_TAG_NULL};
  const thread_t thread = mach_thread_self();
  const kern_return_t rc =
      thread_policy_set(thread, THREAD_AFFINITY_POLICY,
                        reinterpret_cast<thread_policy_t>(&policy),
                        THREAD_AFFINITY_POLICY_COUNT);
  mach_port_deallocate(mach_task_self(), thread);
  if (rc != KERN_SUCCESS) {
    return {CpuAffinityStatus::SystemError, -1, static_cast<int>(rc),
            "thread_policy_set failed"};
  }

  return {CpuAffinityStatus::Applied, -1, 0, "thread affinity tag cleared"};
#else
  return {CpuAffinityStatus::Unsupported, -1, ENOTSUP,
          "CPU affinity is unsupported on this platform"};
#endif
}

const char *cpuAffinityStatusName(CpuAffinityStatus status) noexcept {
  switch (status) {
  case CpuAffinityStatus::Applied:
    return "applied";
  case CpuAffinityStatus::InvalidCpu:
    return "invalid_cpu";
  case CpuAffinityStatus::SystemError:
    return "system_error";
  case CpuAffinityStatus::Unsupported:
    return "unsupported";
  }

  return "unknown";
}

} // namespace astra::utils
