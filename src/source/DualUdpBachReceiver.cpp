#include "astra/source/DualUdpBachReceiver.hpp"

DualUdpBachReceiver::DualUdpBachReceiver(const char *ip_a, uint16_t port_a,
                                         const char *ip_b, uint16_t port_b,
                                         std::size_t batch_size)
    : receiver_a_(ip_a, port_a, batch_size),
      receiver_b_(ip_b, port_b, batch_size) {}

bool DualUdpBachReceiver::next(PacketView &packet) noexcept {
  UdpBachReceiver &first = next_a_first_ ? receiver_a_ : receiver_b_;
  UdpBachReceiver &second = next_a_first_ ? receiver_b_ : receiver_a_;
  uint64_t &first_count = next_a_first_ ? packets_a_ : packets_b_;
  uint64_t &second_count = next_a_first_ ? packets_b_ : packets_a_;

  if (first.next(packet)) {
    ++first_count;
    next_a_first_ = !next_a_first_;
    return true;
  }

  if (second.next(packet)) {
    ++second_count;
    return true;
  }

  return false;
}
