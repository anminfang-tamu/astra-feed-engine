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
  explicit MoldUdpDecoder(astra::symbol::StockDirectory &symbols,
                          BookManager &books, uint8_t channel_id = 0);

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
  DecodeResult drainGapBuffer();

  bool first_packet_seen_{false};
  // bool stage_timing_enabled_{false};
  // DecodeStageTiming last_timing_;
  ChannelState channel_;
  ItchParser parser_;
};
