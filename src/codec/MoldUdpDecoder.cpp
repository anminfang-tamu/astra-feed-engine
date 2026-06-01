#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/utils/DebugTrace.hpp"

#include <cstdint>
#include <span>

MoldUdpDecoder::MoldUdpDecoder(SymbolTable &symbols, BookManager &books,
                               uint8_t channel_id)
    : parser_(symbols, books, channel_id) {
  channel_.channel_id = channel_id;
}

uint16_t MoldUdpDecoder::readU16BE(const std::byte *p) noexcept {
  return (static_cast<uint16_t>(std::to_integer<uint8_t>(p[0])) << 8) |
         static_cast<uint16_t>(std::to_integer<uint8_t>(p[1]));
}

uint64_t MoldUdpDecoder::readU64BE(const std::byte *p) noexcept {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | std::to_integer<uint8_t>(p[i]);
  return v;
}

DecodeResult MoldUdpDecoder::processPacket(const PacketView &packet) {
  if (packet.data == nullptr ||
      packet.size < MoldUdpPacketHeader::kHeaderSize) {
    return {DecodeStatus::PacketTooSmall};
  }

  const uint16_t msg_count = readU16BE(packet.data + 18);
  const uint64_t first_seq = readU64BE(packet.data + 10);

  if (!first_packet_seen_) {
    first_packet_seen_ = true;
    channel_.setSession(reinterpret_cast<const char *>(packet.data));
    channel_.next_expected_seq = first_seq;
  }

  if (msg_count == MoldUdpPacketHeader::kEndOfSessionMessageCount) {
    return {DecodeStatus::EndOfStream};
  }

  // heartbeat or empty packet, just update status and return
  if (msg_count == 0) {
    return {DecodeStatus::Ok};
  }

  const uint64_t expected = channel_.next_expected_seq;
  const uint64_t packet_end = first_seq + msg_count;

  // duplicate or old packet, ignore
  if (packet_end <= expected) {
    return {DecodeStatus::Ok};
  }

  // gap detected
  if (first_seq > expected) {
    const bool already_stale = channel_.status == ChannelHealth::Stale;
    channel_.status = ChannelHealth::GapDetected;

    // FIXME: may not return immediately but buffer the packet and try to
    // recover later
    if (already_stale) {
      channel_.status = ChannelHealth::Stale;
      return {DecodeStatus::Ok, true};
    }
    if (!channel_.gap_buffer.insert(packet.data,
                                    static_cast<uint16_t>(packet.size),
                                    first_seq, msg_count)) {
      channel_.status = ChannelHealth::Stale;
    }

    return {DecodeStatus::Ok, true};
  }

  const uint64_t start_seq = expected > first_seq ? expected : first_seq;

  DecodeResult result = processSequencedPacket(packet.data, packet.size,
                                               first_seq, msg_count, start_seq);

  if (result.status != DecodeStatus::Ok)
    return result;

  if (channel_.status == ChannelHealth::GapDetected)
    channel_.status = ChannelHealth::Recovering;
  channel_.next_expected_seq = packet_end;
  drainGapBuffer();
  if (channel_.status != ChannelHealth::Invalid &&
      channel_.status != ChannelHealth::Stale)
    channel_.status = channel_.gap_buffer.empty() ? ChannelHealth::Good
                                                  : ChannelHealth::Recovering;
  return result;
}

const DecodeStageTiming *MoldUdpDecoder::lastStageTiming() const noexcept {
  // return stage_timing_enabled_ ? &last_timing_ : nullptr;
  return nullptr;
}

void MoldUdpDecoder::setStageTimingEnabled(bool enabled) noexcept {
  (void)enabled;
}

void MoldUdpDecoder::setStockDirectoryWarmup(bool prepare_books,
                                             bool touch_pages) noexcept {
  parser_.setStockDirectoryWarmup(prepare_books, touch_pages);
}

DecodeResult MoldUdpDecoder::processSequencedPacket(const std::byte *data,
                                                    std::size_t size,
                                                    uint64_t first_seq,
                                                    uint16_t msg_count,
                                                    uint64_t start_seq) {
  const uint64_t packet_end = first_seq + msg_count;
  if (start_seq < first_seq || start_seq > packet_end) {
    return {DecodeStatus::InvalidSequence};
  }

  std::size_t offset = MoldUdpPacketHeader::kHeaderSize;
  uint64_t message_seq = first_seq;
  for (uint16_t i = 0; i < msg_count; ++i, ++message_seq) {

    if (offset + 2 > size) {

      return {DecodeStatus::InvalidSize};
    }

    const uint16_t msg_len = readU16BE(data + offset);
    offset += 2;
    if (msg_len == 0 || offset + msg_len > size) {

      return {DecodeStatus::InvalidSize};
    }

    if (message_seq >= start_seq) {
      uint16_t locate = 0;
      if (msg_len >= 3) {
        locate = readU16BE(data + offset + 1);
        channel_.registerStockLocate(locate);
      }

      if (!parser_.handleMessage(
              std::span<const std::byte>(data + offset, msg_len))) {
        return {DecodeStatus::InvalidMessageType};
      }
    }

    offset += msg_len;
  }

  if (offset != size) {
    return {DecodeStatus::InvalidSize};
  }

  return {DecodeStatus::Ok};
}

void MoldUdpDecoder::drainGapBuffer() {
  for (;;) {
    // ASTRA_TRACE("decoder drain gap expected=%llu",
    //             static_cast<unsigned long long>(channel_.next_expected_seq));
    SequencedPacket *packet =
        channel_.gap_buffer.find(channel_.next_expected_seq);
    if (packet == nullptr) {
      // ASTRA_TRACE("decoder drain none expected=%llu",
      //             static_cast<unsigned long
      //             long>(channel_.next_expected_seq));
      return;
    }

    const uint64_t first_seq = packet->seq;
    const uint16_t msg_count = packet->msg_count;
    const uint64_t packet_end = first_seq + msg_count;

    DecodeResult result =
        processSequencedPacket(packet->data.data(), packet->len, first_seq,
                               msg_count, channel_.next_expected_seq);
    channel_.gap_buffer.erase(first_seq);
    if (result.status != DecodeStatus::Ok) {
      channel_.status = ChannelHealth::Invalid;
      return;
    }
    channel_.next_expected_seq = packet_end;
  }
}
