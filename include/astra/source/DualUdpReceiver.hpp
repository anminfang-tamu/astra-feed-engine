#pragma once

#include "astra/source/IMarketDataSource.hpp"
#include "astra/source/UdpReceiver.hpp"

#include <cstdint>

class DualUdpReceiver : public IMarketDataSource {
public:
  DualUdpReceiver(const char *ip_a, uint16_t port_a, const char *ip_b,
                  uint16_t port_b);

  DualUdpReceiver(const DualUdpReceiver &) = delete;
  DualUdpReceiver &operator=(const DualUdpReceiver &) = delete;
  DualUdpReceiver(DualUdpReceiver &&) = delete;
  DualUdpReceiver &operator=(DualUdpReceiver &&) = delete;

  bool next(PacketView &packet) noexcept override;

  uint64_t packetsA() const noexcept { return packets_a_; }
  uint64_t packetsB() const noexcept { return packets_b_; }
  uint64_t errorsA() const noexcept { return receiver_a_.errors(); }
  uint64_t errorsB() const noexcept { return receiver_b_.errors(); }
  uint64_t truncatedA() const noexcept { return receiver_a_.truncated(); }
  uint64_t truncatedB() const noexcept { return receiver_b_.truncated(); }
  uint64_t kernelDropsA() const noexcept { return receiver_a_.kernelDrops(); }
  uint64_t kernelDropsB() const noexcept { return receiver_b_.kernelDrops(); }
  bool dropMetricsEnabled() const noexcept {
    return receiver_a_.dropMetricsEnabled() || receiver_b_.dropMetricsEnabled();
  }

private:
  UdpReceiver receiver_a_;
  UdpReceiver receiver_b_;
  bool next_a_first_{true};
  uint64_t packets_a_{0};
  uint64_t packets_b_{0};
};
