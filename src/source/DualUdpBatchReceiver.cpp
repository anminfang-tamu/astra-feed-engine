#include "astra/source/DualUdpBatchReceiver.hpp"

DualUdpBatchReceiver::DualUdpBatchReceiver(const char *ip_a, uint16_t port_a,
                                           const char *ip_b, uint16_t port_b,
                                           std::size_t batch_size)
    : receiver_a_(ip_a, port_a, batch_size),
      receiver_b_(ip_b, port_b, batch_size) {}

bool DualUdpBatchReceiver::next(PacketView &packet) noexcept {
  UdpBatchReceiver &first = next_a_first_ ? receiver_a_ : receiver_b_;
  UdpBatchReceiver &second = next_a_first_ ? receiver_b_ : receiver_a_;
  uint64_t &first_count = next_a_first_ ? packets_a_ : packets_b_;
  uint64_t &second_count = next_a_first_ ? packets_b_ : packets_a_;
  const uint8_t first_line = next_a_first_ ? 0 : 1;
  const uint8_t second_line = next_a_first_ ? 1 : 0;

  if (first.next(packet)) {
    packet.line_index = first_line;
    ++first_count;
    next_a_first_ = !next_a_first_;
    return true;
  }

  if (second.next(packet)) {
    packet.line_index = second_line;
    ++second_count;
    return true;
  }

  return false;
}
