#pragma once

#include "IMarketDataSource.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

// Single-port, single-RX-queue DPDK source. In dual-feed mode both redundant
// MoldUDP endpoints are filtered and consumed by the same polling thread.
class DpdkReceiver : public IMarketDataSource {
public:
  static constexpr std::size_t kDefaultBurstSize = 32;

  DpdkReceiver(const char *ip, uint16_t port);
  DpdkReceiver(const char *ip_a, uint16_t port_a, const char *ip_b,
               uint16_t port_b);
  ~DpdkReceiver() override;

  DpdkReceiver(const DpdkReceiver &) = delete;
  DpdkReceiver &operator=(const DpdkReceiver &) = delete;
  DpdkReceiver(DpdkReceiver &&) = delete;
  DpdkReceiver &operator=(DpdkReceiver &&) = delete;

  // packet.data points into a DPDK mbuf. It remains valid until the next
  // DpdkReceiver::next() call on this receiver or until receiver destruction.
  bool next(PacketView &packet) override;

  uint64_t packetsA() const noexcept;
  uint64_t packetsB() const noexcept;
  uint64_t filtered() const noexcept;
  uint64_t malformed() const noexcept;
  uint64_t fastPathPackets() const noexcept;
  uint64_t fallbackPathPackets() const noexcept;
  uint64_t polls() const noexcept;
  uint64_t emptyPolls() const noexcept;
  uint64_t imissed() const noexcept;
  uint64_t ierrors() const noexcept;
  uint64_t rxNombuf() const noexcept;
  std::size_t burstSize() const noexcept;
  uint16_t portId() const noexcept;
  uint16_t queueId() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
