#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/protocol/ItchMessageLength.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>

namespace {

inline constexpr std::uint64_t kMinimumFramedItchMessageBytes = 2 + 12;
inline constexpr std::uint64_t kMaxMessagesPerStoredPacket =
    (SequencedPacket::kMaxPacketSize - MoldUdpPacketHeader::kHeaderSize) /
    kMinimumFramedItchMessageBytes;
static_assert(kMaxMessagesPerStoredPacket > 0);

void mergeLatency(DecodeResult &target, const DecodeResult &source) noexcept {
  target.latency_total_ns += source.latency_total_ns;
  target.latency_sample_count += source.latency_sample_count;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((cold, noinline))
#endif
void reportMessageFailure(std::uint64_t packet_first_seq,
                          std::uint64_t message_seq, char message_type,
                          std::uint16_t stock_locate,
                          std::string_view detail) {
  std::cerr << "decoder_message_failure"
            << " status="
            << decodeStatusName(DecodeStatus::InvalidItchMessage)
            << " packet_first_seq=" << packet_first_seq
            << " message_seq=" << message_seq
            << " message_type=" << message_type
            << " stock_locate=" << stock_locate
            << " detail=\"" << detail << "\"\n";
}

} // namespace

MoldUdpDecoder::MoldUdpDecoder(astra::symbol::StockDirectory &symbols,
                               BookManager &books, uint8_t channel_id,
                               std::uint64_t initial_expected_sequence)
    : parser_(symbols, books, channel_id) {
  channel_.channel_id = channel_id;
  channel_.next_expected_seq = initial_expected_sequence;
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
  if (terminal_status_ != DecodeStatus::Ok)
    return {terminal_status_};

  if (packet.data == nullptr ||
      packet.size < MoldUdpPacketHeader::kHeaderSize) {
    return {DecodeStatus::PacketTooSmall};
  }

  const uint16_t msg_count = readU16BE(packet.data + 18);
  const uint64_t first_seq = readU64BE(packet.data + 10);

  // Mold heartbeats and end-of-session markers have no payload. Validate
  // this before binding the process to a session or advancing any state.
  if ((msg_count == 0 ||
      msg_count == MoldUdpPacketHeader::kEndOfSessionMessageCount) &&
      packet.size != MoldUdpPacketHeader::kHeaderSize) {
    return terminalFailure(DecodeStatus::InvalidSize);
  }

  if (!first_packet_seen_) {
    first_packet_seen_ = true;
    channel_.setSession(reinterpret_cast<const char *>(packet.data));
  } else if (!channel_.sessionMatches(
                 reinterpret_cast<const char *>(packet.data))) {
    // One process owns exactly one Mold session.  Do not use generation tags
    // to mask a partial reset of books, roots, directory, and sequencing.
    return terminalFailure(DecodeStatus::InvalidSession);
  }

  if (msg_count == MoldUdpPacketHeader::kEndOfSessionMessageCount) {
    // A transport end marker is authoritative only after the sequenced ITCH
    // stream itself reached System Event C.  Otherwise accepting it would
    // silently discard a missing delete/broken-trade cleanup tail. Require
    // exact sequence continuity and a completely recovered channel before
    // allowing MarketDataEngine to stop normally.
    if (first_seq < channel_.next_expected_seq ||
        (pending_end_sequence_ != 0 &&
         pending_end_sequence_ != first_seq) ||
        channel_.heartbeat_next_seq_high_watermark > first_seq) {
      return terminalFailure(DecodeStatus::InvalidSequence);
    }

    if (first_seq > channel_.next_expected_seq) {
      // In a redundant A/B feed, one line's EOS can arrive before the other
      // line supplies a missing sequenced packet. Retain the advertised end
      // sequence as a high-water mark and let normal gap recovery finish.
      // Once System Event C is reconstructed at this exact sequence,
      // resolvePendingEndOfStream() accepts the pending marker.
      if (channel_.phase == ChannelPhase::EndOfMessages ||
          channel_.status == ChannelHealth::Invalid ||
          channel_.status == ChannelHealth::Stale) {
        return terminalFailure(DecodeStatus::InvalidSequence);
      }
      pending_end_sequence_ = first_seq;
      channel_.heartbeat_next_seq_high_watermark =
          std::max(channel_.heartbeat_next_seq_high_watermark, first_seq);
      channel_.status = ChannelHealth::GapDetected;
      return {DecodeStatus::Ok, true};
    }

    if (channel_.status != ChannelHealth::Good ||
        !channel_.gap_buffer.empty() ||
        channel_.heartbeat_next_seq_high_watermark != 0) {
      return terminalFailure(DecodeStatus::InvalidSequence);
    }
    if (channel_.phase != ChannelPhase::EndOfMessages)
      return terminalFailure(DecodeStatus::IncompleteSession);

    // End-of-stream is sticky: a caller that invokes the decoder again cannot
    // accidentally apply traffic after the accepted terminal boundary.
    terminal_status_ = DecodeStatus::EndOfStream;
    return {terminal_status_};
  }

  // A heartbeat carries the next sequence.  An ahead heartbeat is evidence
  // of missing traffic even though it has no messages to buffer.
  if (msg_count == 0) {
    if (pending_end_sequence_ != 0 &&
        first_seq > pending_end_sequence_) {
      return terminalFailure(DecodeStatus::InvalidSequence);
    }
    if (first_seq > channel_.next_expected_seq) {
      channel_.heartbeat_next_seq_high_watermark =
          std::max(channel_.heartbeat_next_seq_high_watermark, first_seq);
      if (channel_.status != ChannelHealth::Stale &&
          channel_.status != ChannelHealth::Invalid) {
        channel_.status = ChannelHealth::GapDetected;
      }
      return {DecodeStatus::Ok, true};
    }
    if (channel_.heartbeat_next_seq_high_watermark != 0 &&
        channel_.next_expected_seq >=
            channel_.heartbeat_next_seq_high_watermark) {
      channel_.heartbeat_next_seq_high_watermark = 0;
      if (channel_.gap_buffer.empty() &&
          channel_.status != ChannelHealth::Invalid &&
          channel_.status != ChannelHealth::Stale) {
        channel_.status = ChannelHealth::Good;
      }
    }
    return {DecodeStatus::Ok};
  }


  if (first_seq > std::numeric_limits<std::uint64_t>::max() - msg_count) {
    return terminalFailure(DecodeStatus::InvalidSequence);
  }

  const DecodeStatus validation_status =
      validateSequencedPacket(packet.data, packet.size, msg_count);
  if (validation_status != DecodeStatus::Ok)
    return terminalFailure(validation_status);

  const uint64_t expected = channel_.next_expected_seq;
  const uint64_t packet_end = first_seq + msg_count;
  if (pending_end_sequence_ != 0 &&
      packet_end > pending_end_sequence_) {
    return terminalFailure(DecodeStatus::InvalidSequence);
  }

  // duplicate or old packet, ignore
  if (packet_end <= expected) {
    return {DecodeStatus::Ok, false, false};
  }

  // gap detected
  if (first_seq > expected) {
    const bool first_gap = channel_.status != ChannelHealth::GapDetected &&
                           channel_.status != ChannelHealth::Recovering;

    const bool already_stale = channel_.status == ChannelHealth::Stale;
    channel_.status = ChannelHealth::GapDetected;

    if (first_gap) {
      std::cout << "Gap meet channel_expected_seq=" << expected
                << " packet_first_seq=" << first_seq
                << " packet_end_seq=" << packet_end << "\n";
    }

    if (already_stale) {
      channel_.status = ChannelHealth::Stale;
      return {DecodeStatus::Ok, true, false};
    }
    if (packet.size > SequencedPacket::kMaxPacketSize) {
      return terminalFailure(DecodeStatus::InvalidSize);
    }
    const GapBuffer::InsertResult insert_result =
        channel_.gap_buffer.insert(
            packet.data, static_cast<uint16_t>(packet.size), first_seq,
            msg_count, packet.receive_start_ticks);
    if (insert_result ==
        GapBuffer::InsertResult::ConflictingDuplicate) {
      ++channel_.conflicting_buffered_redundant_packets;
      return terminalFailure(DecodeStatus::RedundantFeedMismatch);
    }
    if (insert_result ==
        GapBuffer::InsertResult::DuplicateIdentical) {
      ++channel_.identical_buffered_redundant_packets;
    } else if (insert_result != GapBuffer::InsertResult::Inserted) {
      channel_.status = ChannelHealth::Stale;
    }

    return {DecodeStatus::Ok, true, false};
  }

  const uint64_t start_seq = expected > first_seq ? expected : first_seq;

  DecodeResult result =
      processSequencedPacket(packet.data, packet.size, first_seq, msg_count,
                             start_seq, packet.receive_start_ticks);

  if (result.status != DecodeStatus::Ok) {
    return terminalFailure(result.status);
  }

  if (channel_.status == ChannelHealth::GapDetected)
    channel_.status = ChannelHealth::Recovering;
  channel_.next_expected_seq = packet_end;

  const bool had_gap = channel_.status == ChannelHealth::GapDetected ||
                       channel_.status == ChannelHealth::Recovering;

  DecodeResult drained = drainGapBuffer();
  mergeLatency(result, drained);
  if (drained.status != DecodeStatus::Ok)
    return terminalFailure(drained.status);
  if (channel_.status != ChannelHealth::Invalid &&
      channel_.status != ChannelHealth::Stale) {
    if (channel_.heartbeat_next_seq_high_watermark != 0 &&
        channel_.next_expected_seq >=
            channel_.heartbeat_next_seq_high_watermark) {
      channel_.heartbeat_next_seq_high_watermark = 0;
    }
    const bool resolved =
        channel_.gap_buffer.empty() &&
        channel_.heartbeat_next_seq_high_watermark == 0;
    channel_.status =
        resolved ? ChannelHealth::Good : ChannelHealth::Recovering;

    if (had_gap && resolved) {
      std::cout << "Gap resolve channel_next_seq=" << channel_.next_expected_seq
                << "\n";
    }
  }
  result.had_gap = had_gap;
  const DecodeStatus pending_status = resolvePendingEndOfStream();
  if (pending_status != DecodeStatus::Ok) {
    if (pending_status == DecodeStatus::EndOfStream) {
      result.status = pending_status;
      return result;
    }
    return terminalFailure(pending_status);
  }
  return result;
}

DecodeResult
MoldUdpDecoder::terminalFailure(DecodeStatus status) noexcept {
  channel_.status = ChannelHealth::Invalid;
  terminal_status_ = status;
  return {status};
}

DecodeStatus MoldUdpDecoder::resolvePendingEndOfStream() noexcept {
  if (pending_end_sequence_ == 0 ||
      channel_.next_expected_seq < pending_end_sequence_) {
    return DecodeStatus::Ok;
  }
  if (channel_.next_expected_seq > pending_end_sequence_)
    return DecodeStatus::InvalidSequence;
  if (channel_.status != ChannelHealth::Good ||
      !channel_.gap_buffer.empty() ||
      channel_.heartbeat_next_seq_high_watermark != 0) {
    return DecodeStatus::Ok;
  }
  if (channel_.phase != ChannelPhase::EndOfMessages)
    return DecodeStatus::IncompleteSession;

  pending_end_sequence_ = 0;
  terminal_status_ = DecodeStatus::EndOfStream;
  return terminal_status_;
}

const DecodeStageTiming *MoldUdpDecoder::lastStageTiming() const noexcept {
  // return stage_timing_enabled_ ? &last_timing_ : nullptr;
  return nullptr;
}

void MoldUdpDecoder::setStageTimingEnabled(bool enabled) noexcept {
  (void)enabled;
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
      parser_.handlePrevalidatedMessage(
          std::span<const std::byte>(data + offset, msg_len));
      channel_.phase = parser_.channelPhase();
      if (!parser_.lastError().empty()) [[unlikely]] {
        const char message_type =
            static_cast<char>(std::to_integer<std::uint8_t>(data[offset]));
        const std::uint16_t stock_locate =
            readU16BE(data + offset + 1);
        reportMessageFailure(first_seq, message_seq, message_type,
                             stock_locate, parser_.lastError());
        channel_.status = ChannelHealth::Invalid;
        return {DecodeStatus::InvalidItchMessage};
      }
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

DecodeStatus MoldUdpDecoder::validateSequencedPacket(
    const std::byte *data, std::size_t size,
    uint16_t msg_count) const noexcept {
  // Validate complete Mold framing and every exact ITCH 5.0 message length
  // before applying or buffering the first message. This both prevents partial
  // packet commits and bounds the span of every packet stored for recovery.
  std::size_t validation_offset = MoldUdpPacketHeader::kHeaderSize;
  for (uint16_t i = 0; i < msg_count; ++i) {
    if (validation_offset + 2 > size)
      return DecodeStatus::InvalidSize;
    const uint16_t msg_len = readU16BE(data + validation_offset);
    validation_offset += 2;
    if (msg_len == 0 || msg_len > size - validation_offset)
      return DecodeStatus::InvalidSize;
    const std::uint8_t expected_len =
        astra::protocol::expectedItchMessageLength(
            data[validation_offset]);
    if (expected_len == 0 || msg_len != expected_len)
      return DecodeStatus::InvalidItchMessage;
    validation_offset += msg_len;
  }
  if (validation_offset != size)
    return DecodeStatus::InvalidSize;
  return DecodeStatus::Ok;
}

DecodeResult MoldUdpDecoder::drainGapBuffer() {
  DecodeResult aggregate{DecodeStatus::Ok};
  for (;;) {
    // A bridge packet can advance expected into the middle of a packet that
    // was buffered earlier. Search the bounded set of valid packet starts at
    // or below expected and process only its unseen tail. This also removes a
    // buffered packet that the bridge fully subsumed.
    SequencedPacket *packet = nullptr;
    uint64_t first_seq = 0;
    uint64_t packet_end = 0;
    bool erased_stale_packet = false;
    const uint64_t max_distance =
        std::min(channel_.next_expected_seq, kMaxMessagesPerStoredPacket);
    for (uint64_t distance = 0; distance <= max_distance; ++distance) {
      const uint64_t candidate_seq =
          channel_.next_expected_seq - distance;
      SequencedPacket *candidate = channel_.gap_buffer.find(candidate_seq);
      if (candidate == nullptr)
        continue;

      const uint64_t candidate_end =
          candidate->seq + candidate->msg_count;
      if (candidate_end <= channel_.next_expected_seq) {
        channel_.gap_buffer.erase(candidate->seq);
        erased_stale_packet = true;
        break;
      }

      packet = candidate;
      first_seq = candidate->seq;
      packet_end = candidate_end;
      break;
    }

    if (erased_stale_packet)
      continue;
    if (packet == nullptr) {
      return aggregate;
    }

    const uint64_t receive_start_ticks = packet->receive_start_ticks;
    const uint16_t msg_count = packet->msg_count;

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
