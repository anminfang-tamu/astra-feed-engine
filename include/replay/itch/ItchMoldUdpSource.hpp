#pragma once

#include "astra/protocol/SequencedPacket.hpp"
#include "astra/source/IMarketDataSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

// Reads a Nasdaq TotalView-ITCH 5.0 BinaryFILE and emits valid MoldUDP64
// downstream packets. BinaryFILE does not retain original live packet
// boundaries, sessions, or arrival timing, so batching is intentionally
// synthetic even though the header, block framing, sequence, and payload bytes
// follow the wire protocols exactly.
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
    // Nasdaq BinaryFILE ends with a zero-length record. Strict mode requires
    // that marker. The compatibility mode exists only for independently
    // verified provider files that end physically after the final System Event
    // C without the required marker. It never accepts an arbitrary
    // record-aligned EOF as successful completion.
    enum class CompletionPolicy : uint8_t {
        RequireTerminator,
        AllowEofAfterEndOfMessages,
    };

    enum class CompletionKind : uint8_t {
        None,
        Terminator,
        LegacyEndOfMessagesEof,
    };

    static constexpr uint16_t    kDefaultMsgsPerPacket = 20;
    static constexpr std::size_t kMoldHeaderSize       = 20; // 10+8+2
    // Keep generated UDP payloads within a standard 1,500-byte IPv4 MTU:
    // 1,500 - 20-byte IPv4 header - 8-byte UDP header = 1,472 bytes.
    // The DPDK receive path intentionally rejects fragmented IPv4 packets.
    static constexpr std::size_t kStandardIpv4UdpPayloadBytes = 1'472;
    static constexpr std::size_t kMaxPacketBytes =
        kStandardIpv4UdpPayloadBytes < SequencedPacket::kMaxPacketSize
            ? kStandardIpv4UdpPayloadBytes
            : SequencedPacket::kMaxPacketSize;

    // session must be <= 10 chars; it is right-padded with spaces to exactly 10.
    ItchMoldUdpSource(const std::string &path,
                      std::string        session        = "ASTRA     ",
                      uint16_t           msgs_per_packet = kDefaultMsgsPerPacket,
                      CompletionPolicy   completion_policy =
                          CompletionPolicy::RequireTerminator);

    bool next(PacketView &packet) override;

    bool               isOpen()     const;
    bool               completed()  const noexcept;
    CompletionKind     completionKind() const noexcept;
    uint64_t           nextSequence() const noexcept { return next_seq_; }
    const std::string &lastError()  const;

    // Packetization is synthetic because BinaryFILE does not preserve live
    // datagram boundaries. The sender uses one-message packets only while
    // timestamp-paced replay is active so no later message can be released at
    // an earlier message's timestamp.
    [[nodiscard]] bool
    setMessagesPerPacket(uint16_t msgs_per_packet) noexcept;

private:
    std::ifstream input_;
    std::string   session_;         // exactly 10 chars
    uint16_t      msgs_per_packet_;
    CompletionPolicy completion_policy_;
    uint64_t      next_seq_{1};     // MoldUDP64 sequence number (1-based, per message)
    std::string   last_error_;
    bool          eof_{false};
    bool          completed_{false};
    bool          saw_message_{false};
    bool          last_message_was_end_of_messages_{false};
    CompletionKind completion_kind_{CompletionKind::None};

    std::array<std::byte, kMaxPacketBytes> packet_buf_{};
};
