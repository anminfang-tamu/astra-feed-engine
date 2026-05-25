#pragma once

#include "astra/source/IMarketDataSource.hpp"
#include "astra/source/UdpBachReceiver.hpp"

#include <cstdint>

class DualUdpBachReceiver : public IMarketDataSource {
public:
  DualUdpBachReceiver(
      const char *ip_a, uint16_t port_a, const char *ip_b, uint16_t port_b,
      std::size_t batch_size = UdpBachReceiver::kDefaultBatchSize);

  DualUdpBachReceiver(const DualUdpBachReceiver &) = delete;
  DualUdpBachReceiver &operator=(const DualUdpBachReceiver &) = delete;
  DualUdpBachReceiver(DualUdpBachReceiver &&) = delete;
  DualUdpBachReceiver &operator=(DualUdpBachReceiver &&) = delete;

  bool next(PacketView &packet) noexcept override;

  uint64_t packetsA() const noexcept { return packets_a_; }
  uint64_t packetsB() const noexcept { return packets_b_; }
  uint64_t errorsA() const noexcept { return receiver_a_.errors(); }
  uint64_t errorsB() const noexcept { return receiver_b_.errors(); }
  uint64_t truncatedA() const noexcept { return receiver_a_.truncated(); }
  uint64_t truncatedB() const noexcept { return receiver_b_.truncated(); }
  uint64_t syscallsA() const noexcept { return receiver_a_.syscalls(); }
  uint64_t syscallsB() const noexcept { return receiver_b_.syscalls(); }
  uint64_t kernelDropsA() const noexcept { return receiver_a_.kernelDrops(); }
  uint64_t kernelDropsB() const noexcept { return receiver_b_.kernelDrops(); }

private:
  UdpBachReceiver receiver_a_;
  UdpBachReceiver receiver_b_;
  bool next_a_first_{true};
  uint64_t packets_a_{0};
  uint64_t packets_b_{0};
};
