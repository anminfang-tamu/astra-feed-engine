#pragma once

#include <cstddef>
#include <cstdint>

struct MoldUdpPacketHeader {
  static constexpr std::size_t kSessionSize = 10;
  static constexpr std::size_t kHeaderSize = 20;
  static constexpr uint16_t kEndOfSessionMessageCount = 0xFFFFu;

  char session[kSessionSize]{};
  uint64_t first_sequence{0};
  uint16_t message_count{0};
};
