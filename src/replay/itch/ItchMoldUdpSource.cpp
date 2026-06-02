#include "replay/itch/ItchMoldUdpSource.hpp"

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
                                     uint16_t           msgs_per_packet)
    : input_(path, std::ios::binary),
      session_(std::move(session)),
      msgs_per_packet_(msgs_per_packet) {
    if (!input_.is_open()) {
        last_error_ = "failed to open ITCH file: " + path;
    }
    session_.resize(10, ' '); // pad/truncate to exactly 10 bytes
}

bool ItchMoldUdpSource::next(PacketView &packet) {
    packet = PacketView{};
    if (eof_) return false;

    const uint64_t first_seq = next_seq_;
    uint16_t       count     = 0;
    std::size_t    offset    = kMoldHeaderSize; // leave room for header

    while (count < msgs_per_packet_) {
        // Read the 2-byte big-endian message length that prefixes every ITCH
        // message in the historical file — same framing MoldUDP64 uses per message.
        unsigned char len_bytes[2];
        input_.read(reinterpret_cast<char *>(len_bytes), 2);

        if (input_.gcount() == 0 && input_.eof()) { eof_ = true; break; }
        if (input_.gcount() != 2)                 { eof_ = true; break; }

        const uint16_t msg_len =
            (static_cast<uint16_t>(len_bytes[0]) << 8) | len_bytes[1];
        if (msg_len == 0) { eof_ = true; break; }

        if (offset + 2u + msg_len > kMaxPacketBytes) {
            // Message won't fit in this packet — seek back and flush now.
            // ITCH messages are tiny (<40 bytes) so this only triggers if
            // msgs_per_packet is set absurdly high relative to the buffer.
            input_.seekg(-2, std::ios::cur);
            break;
        }

        // Write [2-byte length][payload] directly into the packet buffer —
        // no intermediate copy, no translation.
        writeU16BE(packet_buf_.data() + offset, msg_len);
        offset += 2;

        input_.read(reinterpret_cast<char *>(packet_buf_.data() + offset),
                    static_cast<std::streamsize>(msg_len));
        if (input_.gcount() != static_cast<std::streamsize>(msg_len)) {
            eof_ = true;
            break;
        }
        offset += msg_len;

        ++count;
        ++next_seq_;
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

bool ItchMoldUdpSource::isOpen() const { return input_.is_open(); }

const std::string &ItchMoldUdpSource::lastError() const { return last_error_; }
