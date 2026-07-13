#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/utils/DebugTrace.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <span>

namespace {

void mergeLatency(DecodeResult &target, const DecodeResult &source) noexcept {
  target.latency_total_ns += source.latency_total_ns;
  target.latency_sample_count += source.latency_sample_count;
}

} // namespace

MoldUdpDecoder::MoldUdpDecoder(astra::symbol::StockDirectory &symbols,
                               BookManager &books, uint8_t channel_id)
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
  const char *packet_session = reinterpret_cast<const char *>(packet.data);

  recordFirstReceived(first_seq, packet.line_index);

  if (channel_.session_initialized &&
      !channel_.sessionMatches(packet_session)) {
    ++channel_.session_mismatch_packets;
    return {DecodeStatus::InvalidSequence};
  }

  if (msg_count == MoldUdpPacketHeader::kEndOfSessionMessageCount) {
    // A future/stale end marker must not terminate a cold decoder before the
    // missing stream head has had a chance to arrive on the redundant line.
    if (first_seq != channel_.next_expected_seq)
      return {DecodeStatus::InvalidSequence};
    if (!channel_.session_initialized)
      channel_.setSession(packet_session);
    return {DecodeStatus::EndOfStream};
  }

  // heartbeat or empty packet, just update status and return
  if (msg_count == 0) {
    if (!channel_.session_initialized &&
        first_seq == channel_.next_expected_seq) {
      channel_.setSession(packet_session);
    }
    return {DecodeStatus::Ok, false, false};
  }

  if (first_seq > std::numeric_limits<uint64_t>::max() - msg_count)
    return {DecodeStatus::InvalidSequence};

  const uint64_t expected = channel_.next_expected_seq;
  const uint64_t packet_end = first_seq + msg_count;

  // duplicate or old packet, ignore
  if (packet_end <= expected) {
    return {DecodeStatus::Ok, false, false};
  }

  // gap detected
  if (first_seq > expected) {
    const bool first_gap = channel_.status != ChannelHealth::GapDetected &&
                           channel_.status != ChannelHealth::Recovering &&
                           channel_.status != ChannelHealth::Stale;

    const bool already_stale = channel_.status == ChannelHealth::Stale;
    channel_.status = ChannelHealth::GapDetected;

    if (first_gap) {
      std::cout << "Gap meet channel_expected_seq=" << expected
                << " packet_first_seq=" << first_seq
                << " packet_end_seq=" << packet_end << "\n";
    }

    // FIXME: may not return immediately but buffer the packet and try to
    // recover later
    if (already_stale) {
      channel_.status = ChannelHealth::Stale;
      return {DecodeStatus::Ok, true, false};
    }
    SequencedPacket *buffered = channel_.gap_buffer.find(first_seq);
    bool should_insert = buffered == nullptr;
    if (buffered != nullptr && channel_.session_initialized) {
      const char *buffered_session =
          reinterpret_cast<const char *>(buffered->data.data());
      if (!channel_.sessionMatches(buffered_session)) {
        ++channel_.session_mismatch_packets;
        should_insert = true;
      }
    }

    // Keep the first redundant copy (and its earlier receive timestamp). Once
    // the active session is known, a matching packet may replace a stale
    // pre-bind copy at the same sequence.
    if (should_insert &&
        (packet.size > SequencedPacket::kMaxPacketSize ||
         !channel_.gap_buffer.insert(
             packet.data, static_cast<uint16_t>(packet.size), first_seq,
             msg_count, packet.receive_start_ticks))) {
      channel_.status = ChannelHealth::Stale;
    }

    return {DecodeStatus::Ok, true, false};
  }

  const uint64_t start_seq = expected > first_seq ? expected : first_seq;

  DecodeResult result =
      processSequencedPacket(packet.data, packet.size, first_seq, msg_count,
                             start_seq, packet.receive_start_ticks);

  if (result.status != DecodeStatus::Ok)
    return result;

  // Bind only after the first in-sequence packet has been structurally
  // validated. A malformed sequence-1 packet must not lock the decoder to a
  // stale session and prevent the redundant copy from recovering it.
  if (!channel_.session_initialized && first_seq <= expected &&
      packet_end > expected) {
    channel_.setSession(packet_session);
  }

  if (channel_.status == ChannelHealth::GapDetected)
    channel_.status = ChannelHealth::Recovering;
  channel_.next_expected_seq = packet_end;

  const bool had_gap = channel_.status == ChannelHealth::GapDetected ||
                       channel_.status == ChannelHealth::Recovering;

  bool rejected_buffered_session = false;
  if (!channel_.gap_buffer.empty()) {
    const uint64_t mismatch_count_before = channel_.session_mismatch_packets;
    DecodeResult drained = drainGapBuffer();
    rejected_buffered_session =
        channel_.session_mismatch_packets != mismatch_count_before;
    mergeLatency(result, drained);
    if (drained.status != DecodeStatus::Ok)
      return drained;
  }
  if (channel_.status != ChannelHealth::Invalid &&
      channel_.status != ChannelHealth::Stale) {
    const bool resolved =
        channel_.gap_buffer.empty() && !rejected_buffered_session;
    channel_.status = resolved ? ChannelHealth::Good
                               : ChannelHealth::Recovering;

    if (had_gap && resolved) {
      std::cout << "Gap resolve channel_next_seq=" << channel_.next_expected_seq
                << "\n";
    }
  }
  return result;
}

const DecodeStageTiming *MoldUdpDecoder::lastStageTiming() const noexcept {
  // return stage_timing_enabled_ ? &last_timing_ : nullptr;
  return nullptr;
}

void MoldUdpDecoder::setStageTimingEnabled(bool enabled) noexcept {
  (void)enabled;
}

void MoldUdpDecoder::recordFirstReceived(uint64_t first_seq,
                                         uint8_t line_index) noexcept {
  if (channel_.first_received_seq == 0)
    channel_.first_received_seq = first_seq;
  if (line_index < ChannelState::kLineCount &&
      channel_.first_received_seq_by_line[line_index] == 0) {
    channel_.first_received_seq_by_line[line_index] = first_seq;
  }
}

DecodeResult MoldUdpDecoder::processSequencedPacket(
    const std::byte *data, std::size_t size, uint64_t first_seq,
    uint16_t msg_count, uint64_t start_seq, uint64_t receive_start_ticks) {
  const uint64_t packet_end = first_seq + msg_count;
  if (start_seq < first_seq || start_seq > packet_end) {
    return {DecodeStatus::InvalidSequence};
  }

  std::size_t offset = MoldUdpPacketHeader::kHeaderSize;
  uint64_t message_seq = first_seq;
  uint64_t processed_messages = 0;
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
      parser_.handleMessage(std::span<const std::byte>(data + offset, msg_len));
      ++processed_messages;
    }

    offset += msg_len;
  }

  DecodeResult result{DecodeStatus::Ok};
  if (receive_start_ticks != 0 && processed_messages != 0) {
    result.latency_total_ns = elapsedRdtscNs(receive_start_ticks, rdtsc());
    result.latency_sample_count = processed_messages;
  }
  return result;
}

DecodeResult MoldUdpDecoder::drainGapBuffer() {
  DecodeResult aggregate{DecodeStatus::Ok};
  for (;;) {
    // ASTRA_TRACE("decoder drain gap expected=%llu",
    //             static_cast<unsigned long
    //             long>(channel_.next_expected_seq));
    SequencedPacket *packet =
        channel_.gap_buffer.find(channel_.next_expected_seq);
    if (packet == nullptr) {
      // ASTRA_TRACE("decoder drain none expected=%llu",
      //             static_cast<unsigned long
      //             long>(channel_.next_expected_seq));
      return aggregate;
    }

    const uint64_t first_seq = packet->seq;
    const uint64_t receive_start_ticks = packet->receive_start_ticks;
    const uint16_t msg_count = packet->msg_count;
    const uint64_t packet_end = first_seq + msg_count;

    const char *packet_session =
        reinterpret_cast<const char *>(packet->data.data());
    if (channel_.session_initialized &&
        !channel_.sessionMatches(packet_session)) {
      ++channel_.session_mismatch_packets;
      channel_.gap_buffer.erase(first_seq);
      channel_.status = ChannelHealth::Recovering;
      // The in-sequence packet that triggered this drain was valid and must
      // retain its latency samples. Drop the stale buffered copy and wait for
      // the matching redundant packet at the still-expected sequence.
      return aggregate;
    }

    DecodeResult result = processSequencedPacket(
        packet->data.data(), packet->len, first_seq, msg_count,
        channel_.next_expected_seq, receive_start_ticks);
    mergeLatency(aggregate, result);
    channel_.gap_buffer.erase(first_seq);
    if (result.status != DecodeStatus::Ok) {
      channel_.status = ChannelHealth::Invalid;
      aggregate.status = result.status;
      return aggregate;
    }
    channel_.next_expected_seq = packet_end;
  }
}
