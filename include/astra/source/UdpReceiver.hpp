#pragma once

#include "IMarketDataSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/uio.h>
#endif

class UdpReceiver : public IMarketDataSource {
public:
  UdpReceiver(const char *ip, uint16_t port);
  ~UdpReceiver();

  // Non-copyable, non-movable: owns a file descriptor.
  UdpReceiver(const UdpReceiver &) = delete;
  UdpReceiver &operator=(const UdpReceiver &) = delete;
  UdpReceiver(UdpReceiver &&) = delete;
  UdpReceiver &operator=(UdpReceiver &&) = delete;

  bool next(PacketView &packet) noexcept override;

  uint64_t errors() const noexcept { return error_count_; }
  uint64_t truncated() const noexcept { return truncated_count_; }
  uint64_t kernelDrops() const noexcept { return kernel_drop_count_; }
  bool dropMetricsEnabled() const noexcept { return drop_metrics_enabled_; }

  int fd() const noexcept { return fd_; }

private:
  static constexpr std::size_t kMaxPacketSize = 2048;

#ifdef __linux__
  bool nextWithDropMetrics(PacketView &packet,
                           uint64_t receive_start_ticks) noexcept;
  void recordKernelDrops(const struct msghdr &msg) noexcept;
#endif

  int fd_{-1};
  alignas(64) std::byte buf_[kMaxPacketSize];
#ifdef __linux__
  alignas(struct cmsghdr) std::array<std::byte, 64> control_{};
#endif

  uint64_t error_count_{0};
  uint64_t truncated_count_{0};
  uint64_t kernel_drop_count_{0};
#ifdef __linux__
  uint32_t last_kernel_drop_count_{0};
#endif
  bool drop_metrics_enabled_{false};
};
