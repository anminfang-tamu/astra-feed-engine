#include "replay/itch/ItchMoldUdpSource.hpp"

#include "astra/core/Time.hpp"
#include "astra/protocol/ItchMessageLength.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>

namespace {

void writeU16BE(std::byte *buf, uint16_t v) {
    buf[0] = static_cast<std::byte>((v >> 8) & 0xFFu);
    buf[1] = static_cast<std::byte>( v       & 0xFFu);
}

void writeU64BE(std::byte *buf, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<std::byte>(v & 0xFFu);
        v >>= 8;
    }
}

} // namespace

ItchMoldUdpSource::ItchMoldUdpSource(const std::string &path,
                                     std::string        session,
                                     uint16_t           msgs_per_packet,
                                     CompletionPolicy   completion_policy)
    : input_(path, std::ios::binary),
      session_(std::move(session)),
      msgs_per_packet_(msgs_per_packet),
      completion_policy_(completion_policy) {
    if (!input_.is_open()) {
        last_error_ = "failed to open ITCH file: " + path;
        eof_ = true;
    } else if (session_.size() > 10) {
        last_error_ = "MoldUDP64 session must not exceed 10 bytes";
        eof_ = true;
    } else if (msgs_per_packet_ == 0) {
        last_error_ = "msgs_per_packet must be greater than zero";
        eof_ = true;
    }
    session_.resize(10, ' '); // pad/truncate to exactly 10 bytes
}

bool ItchMoldUdpSource::next(PacketView &packet) {
    packet = PacketView{};
    packet.receive_start_ticks = rdtsc();
    if (eof_) return false;

    const uint64_t first_seq = next_seq_;
    uint16_t       count     = 0;
    std::size_t    offset    = kMoldHeaderSize; // leave room for header

    // Always inspect the BinaryFILE suffix immediately after System Event C,
    // even when C filled the configured packet. This lets the caller replace
    // ordinary pacing/heartbeats with MoldUDP64 EOS as soon as the last data
    // datagram has been emitted.
    while (count < msgs_per_packet_ || last_message_was_end_of_messages_) {
        // Read the 2-byte big-endian message length that prefixes every ITCH
        // message in the historical file — same framing MoldUDP64 uses per message.
        unsigned char len_bytes[2];
        input_.read(reinterpret_cast<char *>(len_bytes), 2);

        if (input_.gcount() == 0 && input_.eof()) {
            eof_ = true;
            if (completion_policy_ ==
                    CompletionPolicy::AllowEofAfterEndOfMessages &&
                last_message_was_end_of_messages_) {
                completed_ = true;
                completion_kind_ =
                    CompletionKind::LegacyEndOfMessagesEof;
            } else {
                last_error_ =
                    "physical EOF before zero-length BinaryFILE terminator";
            }
            break;
        }
        if (input_.gcount() != 2) {
            eof_ = true;
            last_error_ = "truncated ITCH message-length prefix";
            break;
        }

        const uint16_t msg_len =
            (static_cast<uint16_t>(len_bytes[0]) << 8) | len_bytes[1];
        if (msg_len == 0) {
            eof_ = true;
            if (input_.peek() != std::char_traits<char>::eof()) {
                last_error_ =
                    "data follows zero-length BinaryFILE terminator";
            } else if (!last_message_was_end_of_messages_) {
                last_error_ =
                    "BinaryFILE terminator before ITCH System Event C";
            } else {
                completed_ = true;
                completion_kind_ = CompletionKind::Terminator;
            }
            break;
        }

        if (last_message_was_end_of_messages_) {
            eof_ = true;
            last_error_ = "ITCH record follows System Event C";
            break;
        }

        if (offset + 2u + msg_len > kMaxPacketBytes) {
            // Flush existing messages and retry this message in the next
            // packet. A single message that cannot fit is an input error.
            if (count == 0) {
                eof_ = true;
                last_error_ = "ITCH message is too large for a MoldUDP packet";
                break;
            }
            input_.seekg(-2, std::ios::cur);
            break;
        }

        // Write [2-byte length][payload] directly into the packet buffer —
        // no intermediate copy, no translation.
        const std::size_t record_offset = offset;
        writeU16BE(packet_buf_.data() + offset, msg_len);
        offset += 2;

        input_.read(reinterpret_cast<char *>(packet_buf_.data() + offset),
                    static_cast<std::streamsize>(msg_len));
        if (input_.gcount() != static_cast<std::streamsize>(msg_len)) {
            eof_ = true;
            last_error_ = "truncated ITCH message payload";
            offset = record_offset;
            break;
        }

        const std::uint8_t expected_length =
            astra::protocol::expectedItchMessageLength(
                packet_buf_[offset]);
        if (expected_length == 0) {
            eof_ = true;
            last_error_ = "unsupported ITCH message type";
            offset = record_offset;
            break;
        }
        if (msg_len != expected_length) {
            eof_ = true;
            last_error_ = "invalid ITCH message length";
            offset = record_offset;
            break;
        }

        const bool is_system_event =
            static_cast<char>(packet_buf_[offset]) == 'S';
        if (is_system_event &&
            (packet_buf_[offset + 1] != std::byte{0} ||
             packet_buf_[offset + 2] != std::byte{0})) {
            eof_ = true;
            last_error_ = "System Event stock locate is not zero";
            offset = record_offset;
            break;
        }

        const bool is_start_of_messages =
            is_system_event &&
            static_cast<char>(packet_buf_[offset + 11]) == 'O';
        if (!saw_message_ && !is_start_of_messages) {
            eof_ = true;
            last_error_ =
                "first ITCH record is not System Event O";
            offset = record_offset;
            break;
        }

        saw_message_ = true;
        last_message_was_end_of_messages_ =
            is_system_event &&
            static_cast<char>(packet_buf_[offset + 11]) == 'C';
        const bool is_replay_timing_boundary =
            is_system_event &&
            (static_cast<char>(packet_buf_[offset + 11]) == 'S' ||
             static_cast<char>(packet_buf_[offset + 11]) == 'Q');
        offset += msg_len;

        ++count;
        ++next_seq_;
        // The sender applies its optional SS pause and SS-to-SQ pacing only
        // after a complete datagram has been transmitted. End these datagrams
        // at the lifecycle event so no post-event message can cross the timing
        // boundary before the sender observes it.
        if (is_replay_timing_boundary)
            break;
    }

    if (count == 0) return false;

    // Stamp the MoldUDP64 header now that we know the final message count.
    std::memcpy(packet_buf_.data(), session_.data(), 10);
    writeU64BE(packet_buf_.data() + 10, first_seq);
    writeU16BE(packet_buf_.data() + 18, count);

    packet.data = packet_buf_.data();
    packet.size = offset;
    return true;
}

bool ItchMoldUdpSource::isOpen() const {
    return input_.is_open() && last_error_.empty();
}

bool ItchMoldUdpSource::completed() const noexcept { return completed_; }

bool ItchMoldUdpSource::setMessagesPerPacket(
    uint16_t msgs_per_packet) noexcept {
    if (msgs_per_packet == 0)
        return false;
    msgs_per_packet_ = msgs_per_packet;
    return true;
}

ItchMoldUdpSource::CompletionKind
ItchMoldUdpSource::completionKind() const noexcept {
    return completion_kind_;
}

const std::string &ItchMoldUdpSource::lastError() const { return last_error_; }
