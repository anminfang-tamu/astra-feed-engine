#pragma once

#include "IMarketDataSource.hpp"

#include <cstddef>
#include <cstdint>

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

  int fd() const noexcept { return fd_; }

private:
  static constexpr std::size_t kMaxPacketSize = 2048;

  int fd_{-1};
  alignas(64) std::byte buf_[kMaxPacketSize];

  uint64_t error_count_{0};
  uint64_t truncated_count_{0};
};
