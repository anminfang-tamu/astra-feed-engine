#pragma once

#include "astra/source/IMarketDataSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

// Reads a NASDAQ ITCH 5.0 binary file and emits MoldUDP64-framed UDP packets,
// exactly matching what a real NASDAQ multicast feed sends on the wire.
//
// MoldUDP64 packet layout (big-endian):
//   [10 bytes] session identifier (ASCII, space-padded)
//   [ 8 bytes] sequence number of first ITCH message in this packet
//   [ 2 bytes] message count
//   per message:
//     [ 2 bytes] message length
//     [ N bytes] raw ITCH 5.0 message bytes
class ItchMoldUdpSource : public IMarketDataSource {
public:
    static constexpr uint16_t    kDefaultMsgsPerPacket = 20;
    static constexpr std::size_t kMoldHeaderSize       = 20; // 10+8+2
    static constexpr std::size_t kMaxPacketBytes       = 65535;

    // session must be <= 10 chars; it is right-padded with spaces to exactly 10.
    ItchMoldUdpSource(const std::string &path,
                      std::string        session        = "ASTRA     ",
                      uint16_t           msgs_per_packet = kDefaultMsgsPerPacket);

    bool next(PacketView &packet) override;

    bool               isOpen()     const;
    const std::string &lastError()  const;

private:
    std::ifstream input_;
    std::string   session_;         // exactly 10 chars
    uint16_t      msgs_per_packet_;
    uint64_t      next_seq_{1};     // MoldUDP64 sequence number (1-based, per message)
    std::string   last_error_;
    bool          eof_{false};

    std::array<std::byte, kMaxPacketBytes> packet_buf_{};
};
