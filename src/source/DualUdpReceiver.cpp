#include "astra/source/DualUdpReceiver.hpp"

#include <poll.h>

DualUdpReceiver::DualUdpReceiver(const char *ip_a, uint16_t port_a,
                                 const char *ip_b, uint16_t port_b)
    : receiver_a_(ip_a, port_a), receiver_b_(ip_b, port_b) {}

bool DualUdpReceiver::next(PacketView &packet) noexcept {
  UdpReceiver &first = next_a_first_ ? receiver_a_ : receiver_b_;
  UdpReceiver &second = next_a_first_ ? receiver_b_ : receiver_a_;
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

  struct pollfd fds[2] = {
      {.fd = receiver_a_.fd(), .events = POLLIN, .revents = 0},
      {.fd = receiver_b_.fd(), .events = POLLIN, .revents = 0},
  };

  if (::poll(fds, 2, 0) <= 0)
    return false;

  if (next_a_first_) {
    if ((fds[0].revents & POLLIN) != 0 && receiver_a_.next(packet)) {
      ++packets_a_;
      next_a_first_ = false;
      return true;
    }
    if ((fds[1].revents & POLLIN) != 0 && receiver_b_.next(packet)) {
      ++packets_b_;
      return true;
    }
  } else {
    if ((fds[1].revents & POLLIN) != 0 && receiver_b_.next(packet)) {
      ++packets_b_;
      next_a_first_ = true;
      return true;
    }
    if ((fds[0].revents & POLLIN) != 0 && receiver_a_.next(packet)) {
      ++packets_a_;
      return true;
    }
  }

  return false;
}
