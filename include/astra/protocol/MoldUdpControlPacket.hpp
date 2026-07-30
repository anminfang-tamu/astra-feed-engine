#pragma once

#include "astra/protocol/PacketHeader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace astra::protocol {

using MoldUdpControlPacket =
    std::array<std::byte, MoldUdpPacketHeader::kHeaderSize>;

inline MoldUdpControlPacket
makeMoldControlPacket(std::string_view session,
                      std::uint64_t next_sequence,
                      std::uint16_t message_count) noexcept {
  MoldUdpControlPacket packet{};
  for (std::size_t index = 0;
       index < MoldUdpPacketHeader::kSessionSize; ++index) {
    packet[index] =
        index < session.size()
            ? static_cast<std::byte>(
                  static_cast<unsigned char>(session[index]))
            : std::byte{' '};
  }
  for (int index = 7; index >= 0; --index) {
    packet[MoldUdpPacketHeader::kSessionSize +
           static_cast<std::size_t>(index)] =
        static_cast<std::byte>(next_sequence & 0xffu);
    next_sequence >>= 8;
  }
  packet[18] = static_cast<std::byte>(message_count >> 8);
  packet[19] = static_cast<std::byte>(message_count & 0xffu);
  return packet;
}

inline MoldUdpControlPacket
makeMoldHeartbeatPacket(std::string_view session,
                        std::uint64_t next_sequence) noexcept {
  return makeMoldControlPacket(session, next_sequence, 0);
}

inline MoldUdpControlPacket
makeMoldEndOfSessionPacket(std::string_view session,
                           std::uint64_t next_sequence) noexcept {
  return makeMoldControlPacket(
      session, next_sequence,
      MoldUdpPacketHeader::kEndOfSessionMessageCount);
}

} // namespace astra::protocol
