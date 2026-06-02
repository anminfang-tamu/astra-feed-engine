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

private:
  UdpReceiver receiver_a_;
  UdpReceiver receiver_b_;
  bool next_a_first_{true};
  uint64_t packets_a_{0};
  uint64_t packets_b_{0};
};
