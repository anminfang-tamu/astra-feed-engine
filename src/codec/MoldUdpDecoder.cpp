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
  if (stage_timing_enabled_) {
    last_timing_.clear();
    parser_.setStageTiming(&last_timing_);
  } else {
    parser_.setStageTiming(nullptr);
  }

  const uint64_t mold_start_ns = stage_timing_enabled_ ? nowNs() : 0;
  ASTRA_TRACE("decoder processPacket enter data=%p size=%zu",
              static_cast<const void *>(packet.data), packet.size);
  if (packet.data == nullptr ||
      packet.size < MoldUdpPacketHeader::kHeaderSize) {
    ASTRA_TRACE("decoder packet too small size=%zu", packet.size);
    recordMoldParseSince(mold_start_ns);
    return {DecodeStatus::PacketTooSmall};
  }

  const uint16_t msg_count = readU16BE(packet.data + 18);
  ASTRA_TRACE("decoder header msg_count=%u", msg_count);

  if (msg_count == MoldUdpPacketHeader::kEndOfSessionMessageCount) {
    ASTRA_TRACE("decoder end of stream");
    recordMoldParseSince(mold_start_ns);
    return {DecodeStatus::EndOfStream};
  }

  const uint64_t first_seq = readU64BE(packet.data + 10);
  if (!first_packet_seen_) {
    first_packet_seen_ = true;
    channel_.setSession(reinterpret_cast<const char *>(packet.data));
    channel_.next_expected_seq = first_seq;
    ASTRA_TRACE("decoder first packet channel=%u first_seq=%llu",
                channel_.channel_id,
                static_cast<unsigned long long>(first_seq));
  }

  if (msg_count == 0) {
    ASTRA_TRACE("decoder heartbeat first_seq=%llu",
                static_cast<unsigned long long>(first_seq));
    recordMoldParseSince(mold_start_ns);
    return {DecodeStatus::Ok};
  }

  const uint64_t expected = channel_.next_expected_seq;
  const uint64_t packet_end = first_seq + msg_count;
  ASTRA_TRACE("decoder seq first=%llu expected=%llu end=%llu count=%u status=%d",
              static_cast<unsigned long long>(first_seq),
              static_cast<unsigned long long>(expected),
              static_cast<unsigned long long>(packet_end), msg_count,
              static_cast<int>(channel_.status));

  if (first_seq > expected) {
    const bool already_stale = channel_.status == ChannelHealth::Stale;
    channel_.status = ChannelHealth::GapDetected;
    if (stage_timing_enabled_)
      ++last_timing_.gap_packets;
    ASTRA_TRACE("decoder gap detected first=%llu expected=%llu count=%u",
                static_cast<unsigned long long>(first_seq),
                static_cast<unsigned long long>(expected), msg_count);
    if (already_stale) {
      channel_.status = ChannelHealth::Stale;
      if (stage_timing_enabled_)
        ++last_timing_.stale_gap_dropped_packets;
      recordMoldParseSince(mold_start_ns);
      return {DecodeStatus::Ok, true};
    }
    if (channel_.gap_buffer.insert(packet.data,
                                   static_cast<uint16_t>(packet.size),
                                   first_seq, msg_count)) {
      if (stage_timing_enabled_)
        ++last_timing_.gap_buffered_packets;
    } else {
      if (stage_timing_enabled_)
        ++last_timing_.gap_buffer_insert_failed_packets;
      channel_.status = ChannelHealth::Stale;
      ASTRA_TRACE("decoder gap buffer insert failed first=%llu",
                  static_cast<unsigned long long>(first_seq));
    }
    recordMoldParseSince(mold_start_ns);
    return {DecodeStatus::Ok, true};
  }

  if (packet_end <= expected) {
    if (stage_timing_enabled_)
      ++last_timing_.old_packets;
    ASTRA_TRACE("decoder duplicate/old first=%llu end=%llu expected=%llu",
                static_cast<unsigned long long>(first_seq),
                static_cast<unsigned long long>(packet_end),
                static_cast<unsigned long long>(expected));
    recordMoldParseSince(mold_start_ns);
    return {DecodeStatus::Ok};
  }

  const uint64_t start_seq = expected > first_seq ? expected : first_seq;
  if (stage_timing_enabled_)
    ++last_timing_.sequenced_packets;
  recordMoldParseSince(mold_start_ns);
  DecodeResult result =
      processSequencedPacket(packet.data, packet.size, first_seq, msg_count,
                             start_seq);
  ASTRA_TRACE("decoder processSequencedPacket result status=%d",
              static_cast<int>(result.status));
  if (result.status != DecodeStatus::Ok) return result;

  if (channel_.status == ChannelHealth::GapDetected)
    channel_.status = ChannelHealth::Recovering;
  channel_.next_expected_seq = packet_end;
  drainGapBuffer();
  if (channel_.status != ChannelHealth::Invalid &&
      channel_.status != ChannelHealth::Stale)
    channel_.status =
        channel_.gap_buffer.empty() ? ChannelHealth::Good
                                    : ChannelHealth::Recovering;
  return result;
}

const DecodeStageTiming *MoldUdpDecoder::lastStageTiming() const noexcept {
  return stage_timing_enabled_ ? &last_timing_ : nullptr;
}

void MoldUdpDecoder::setStageTimingEnabled(bool enabled) noexcept {
  stage_timing_enabled_ = enabled;
  if (!enabled)
    parser_.setStageTiming(nullptr);
}

void MoldUdpDecoder::recordMoldParseSince(uint64_t start_ns) noexcept {
  if (!stage_timing_enabled_)
    return;

  const uint64_t end_ns = nowNs();
  if (end_ns >= start_ns)
    last_timing_.mold_parse_ns += end_ns - start_ns;
}

DecodeResult MoldUdpDecoder::processSequencedPacket(const std::byte *data,
                                                    std::size_t size,
                                                    uint64_t first_seq,
                                                    uint16_t msg_count,
                                                    uint64_t start_seq) {
  ASTRA_TRACE("decoder processSequenced enter first=%llu count=%u start=%llu size=%zu",
              static_cast<unsigned long long>(first_seq), msg_count,
              static_cast<unsigned long long>(start_seq), size);
  std::size_t offset = MoldUdpPacketHeader::kHeaderSize;
  uint64_t message_seq = first_seq;
  for (uint16_t i = 0; i < msg_count; ++i, ++message_seq) {
    const uint64_t mold_start_ns = stage_timing_enabled_ ? nowNs() : 0;
    if (offset + 2 > size) {
      ASTRA_TRACE("decoder invalid size before len i=%u offset=%zu size=%zu",
                  i, offset, size);
      recordMoldParseSince(mold_start_ns);
      return {DecodeStatus::InvalidSize};
    }

    const uint16_t msg_len = readU16BE(data + offset);
    offset += 2;
    if (msg_len == 0 || offset + msg_len > size) {
      ASTRA_TRACE("decoder invalid msg len i=%u len=%u offset=%zu size=%zu",
                  i, msg_len, offset, size);
      recordMoldParseSince(mold_start_ns);
      return {DecodeStatus::InvalidSize};
    }

    if (message_seq >= start_seq) {
      const char type =
          msg_len > 0 ? static_cast<char>(std::to_integer<uint8_t>(data[offset]))
                      : '?';
      uint16_t locate = 0;
      if (msg_len >= 3) {
        locate = readU16BE(data + offset + 1);
        channel_.registerStockLocate(locate);
      }
      recordMoldParseSince(mold_start_ns);
      ASTRA_TRACE("decoder dispatch seq=%llu idx=%u type=%c locate=%u len=%u offset=%zu",
                  static_cast<unsigned long long>(message_seq), i, type,
                  locate, msg_len, offset);
      if (stage_timing_enabled_)
        ++last_timing_.messages;
      const uint64_t itch_start_ns = stage_timing_enabled_ ? nowNs() : 0;
      parser_.handleMessage(std::span<const std::byte>(data + offset, msg_len));
      if (stage_timing_enabled_) {
        const uint64_t itch_end_ns = nowNs();
        if (itch_end_ns >= itch_start_ns)
          last_timing_.itch_total_ns += itch_end_ns - itch_start_ns;
        if (parser_.lastMessageSkipped())
          ++last_timing_.skipped_messages;
      }
      ASTRA_TRACE("decoder dispatched seq=%llu type=%c locate=%u",
                  static_cast<unsigned long long>(message_seq), type, locate);
    } else {
      recordMoldParseSince(mold_start_ns);
    }

    offset += msg_len;
  }

  return {DecodeStatus::Ok};
}

void MoldUdpDecoder::drainGapBuffer() {
  for (;;) {
    ASTRA_TRACE("decoder drain gap expected=%llu",
                static_cast<unsigned long long>(channel_.next_expected_seq));
    SequencedPacket *packet =
        channel_.gap_buffer.find(channel_.next_expected_seq);
    if (packet == nullptr) {
      ASTRA_TRACE("decoder drain none expected=%llu",
                  static_cast<unsigned long long>(channel_.next_expected_seq));
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
