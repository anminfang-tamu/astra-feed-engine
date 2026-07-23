#pragma once

#include "astra/channel/ChannelState.hpp"
#include "astra/codec/DecodeResult.hpp"
#include "astra/codec/IPacketProcessor.hpp"
#include "astra/parser/ItchParser.hpp"
#include "astra/protocol/PacketHeader.hpp"
#include "astra/source/PacketView.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <cstddef>
#include <cstdint>

// Strips the MoldUDP64 header, iterates each ITCH 5.0 message in the datagram,
// and hands them to ItchParser which writes directly into BookManager.
// No intermediate structs — raw bytes go straight to the order book.
class MoldUdpDecoder : public IPacketProcessor {
public:
  // A decoder never infers continuity from the first datagram it happens to
  // receive. Full-session NASDAQ replay starts at 1; a re-numbered test stream
  // that still contains the complete lifecycle may provide its authoritative
  // first sequence explicitly. This option does not restore mid-session book
  // or parser state.
  static constexpr std::uint64_t kDefaultInitialSequence = 1;

  explicit MoldUdpDecoder(astra::symbol::StockDirectory &symbols,
                          BookManager &books, uint8_t channel_id = 0,
                          std::uint64_t initial_expected_sequence =
                              kDefaultInitialSequence);

  DecodeResult processPacket(const PacketView &packet) override;
  const DecodeStageTiming *lastStageTiming() const noexcept override;
  void setStageTimingEnabled(bool enabled) noexcept;
  const ChannelState &channelState() const noexcept { return channel_; }
  ChannelState &channelState() noexcept { return channel_; }

private:
  static uint16_t readU16BE(const std::byte *p) noexcept;
  static uint64_t readU64BE(const std::byte *p) noexcept;
  void recordMoldParseSince(uint64_t start_ns) noexcept;

  DecodeResult processSequencedPacket(const std::byte *data, std::size_t size,
                                      uint64_t first_seq, uint16_t msg_count,
                                      uint64_t start_seq,
                                      uint64_t receive_start_ticks);
  DecodeStatus validateSequencedPacket(const std::byte *data, std::size_t size,
                                       uint16_t msg_count) const noexcept;
  DecodeResult drainGapBuffer();
  DecodeResult terminalFailure(DecodeStatus status) noexcept;

  bool first_packet_seen_{false};
  DecodeStatus terminal_status_{DecodeStatus::Ok};
  // bool stage_timing_enabled_{false};
  // DecodeStageTiming last_timing_;
  ChannelState channel_;
  ItchParser parser_;
};
