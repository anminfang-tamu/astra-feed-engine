#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelHealth.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/protocol/MoldUdpControlPacket.hpp"
#include "astra/source/PacketView.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;

void appendU8(Bytes &b, uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
void appendU16(Bytes &b, uint16_t v) {
  appendU8(b, static_cast<uint8_t>(v >> 8));
  appendU8(b, static_cast<uint8_t>(v & 0xFF));
}
void appendU32(Bytes &b, uint32_t v) {
  appendU8(b, static_cast<uint8_t>(v >> 24));
  appendU8(b, static_cast<uint8_t>((v >> 16) & 0xFF));
  appendU8(b, static_cast<uint8_t>((v >> 8) & 0xFF));
  appendU8(b, static_cast<uint8_t>(v & 0xFF));
}
void appendU48(Bytes &b, uint64_t v) {
  for (int s = 40; s >= 0; s -= 8)
    appendU8(b, static_cast<uint8_t>((v >> s) & 0xFF));
}
void appendU64(Bytes &b, uint64_t v) {
  for (int s = 56; s >= 0; s -= 8)
    appendU8(b, static_cast<uint8_t>((v >> s) & 0xFF));
}
void appendStock(Bytes &b, std::string_view sym) {
  for (std::size_t i = 0; i < 8; ++i)
    appendU8(b, i < sym.size() ? static_cast<uint8_t>(sym[i]) : ' ');
}

Bytes addMessage(uint16_t locate, uint64_t order_id, char side, uint32_t qty,
                 std::string_view sym, uint32_t price) {
  Bytes b;
  appendU8(b, 'A');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendU64(b, order_id);
  appendU8(b, static_cast<uint8_t>(side));
  appendU32(b, qty);
  appendStock(b, sym);
  appendU32(b, price);
  return b;
}

Bytes executeMessage(uint16_t locate, uint64_t order_id,
                     uint32_t executed_qty, uint64_t match = 9001) {
  Bytes b;
  appendU8(b, 'E');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendU64(b, order_id);
  appendU32(b, executed_qty);
  appendU64(b, match);
  return b;
}

Bytes stockDirectoryMessage(uint16_t locate, std::string_view sym) {
  Bytes b;
  appendU8(b, 'R');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendStock(b, sym);
  appendU8(b, 'Q');
  appendU8(b, 'N');
  appendU32(b, 100);
  appendU8(b, 'N');
  appendU8(b, 'C');
  appendU8(b, ' ');
  appendU8(b, ' ');
  appendU8(b, 'P');
  while (b.size() < 39) appendU8(b, ' ');
  return b;
}

Bytes systemEventMessage(char event_code) {
  Bytes b;
  appendU8(b, 'S');
  appendU16(b, 0);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendU8(b, static_cast<uint8_t>(event_code));
  return b;
}

Bytes moldPacket(uint64_t first_seq, std::span<const Bytes> messages,
                 std::string_view session = "ASTRA     ") {
  Bytes b;
  for (std::size_t index = 0; index < 10; ++index) {
    appendU8(b, index < session.size() ? static_cast<uint8_t>(session[index])
                                       : static_cast<uint8_t>(' '));
  }
  appendU64(b, first_seq);
  appendU16(b, static_cast<uint16_t>(messages.size()));
  for (const Bytes &message : messages) {
    appendU16(b, static_cast<uint16_t>(message.size()));
    b.insert(b.end(), message.begin(), message.end());
  }
  return b;
}

Bytes endOfSessionPacket(uint64_t next_seq,
                         std::string_view session = "ASTRA     ") {
  const std::span<const Bytes> no_messages;
  Bytes packet = moldPacket(next_seq, no_messages, session);
  packet[18] = std::byte{0xff};
  packet[19] = std::byte{0xff};
  return packet;
}

PacketView view(const Bytes &b) {
  return {b.data(), b.size(), rdtsc()};
}

} // namespace

TEST(MoldUdpDecoderTest,
     FirstObservedAheadPacketDoesNotRebaseExpectedSequence) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes directory = stockDirectoryMessage(1, "AAPL");
  const Bytes ahead =
      moldPacket(2, std::span<const Bytes>(&directory, 1));

  const DecodeResult gap = decoder.processPacket(view(ahead));

  EXPECT_EQ(gap.status, DecodeStatus::Ok);
  EXPECT_TRUE(gap.had_gap);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
  EXPECT_EQ(decoder.channelState().gap_buffer.size(), 1u);
  EXPECT_EQ(symbols.size(), 0u);

  const Bytes start = systemEventMessage('O');
  const DecodeResult recovered =
      decoder.processPacket(view(moldPacket(
          1, std::span<const Bytes>(&start, 1))));

  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_TRUE(recovered.had_gap);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 3u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());
  EXPECT_EQ(symbols.size(), 1u);
}

TEST(MoldUdpDecoderTest, ExplicitInitialSequenceIsHonored) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  constexpr std::uint64_t initial_sequence = 42;
  MoldUdpDecoder decoder(symbols, books, 3, initial_sequence);

  const Bytes start = systemEventMessage('O');
  const DecodeResult result =
      decoder.processPacket(view(moldPacket(
          initial_sequence, std::span<const Bytes>(&start, 1))));

  EXPECT_EQ(result.status, DecodeStatus::Ok);
  EXPECT_FALSE(result.had_gap);
  EXPECT_EQ(decoder.channelState().next_expected_seq,
            initial_sequence + 1);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
}

TEST(MoldUdpDecoderTest,
     StartupHeartbeatBindsSessionWithoutAdvancingOrSampling) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);
  const auto heartbeat =
      astra::protocol::makeMoldHeartbeatPacket("ASTRA", 1);

  const PacketView heartbeat_view{
      heartbeat.data(), heartbeat.size(), rdtsc()};
  const DecodeResult first = decoder.processPacket(heartbeat_view);
  const DecodeResult duplicate = decoder.processPacket(heartbeat_view);

  EXPECT_EQ(first.status, DecodeStatus::Ok);
  EXPECT_EQ(first.latency_sample_count, 0u);
  EXPECT_EQ(duplicate.status, DecodeStatus::Ok);
  EXPECT_EQ(duplicate.latency_sample_count, 0u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const DecodeResult setup =
      decoder.processPacket(view(moldPacket(1, setup_messages)));

  EXPECT_EQ(setup.status, DecodeStatus::Ok);
  EXPECT_EQ(setup.latency_sample_count, 2u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 3u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(symbols.size(), 1u);
  EXPECT_NE(books.getOrderBook(1), nullptr);
}

TEST(MoldUdpDecoderTest, FirstAheadHeartbeatDoesNotRebaseExpectedSequence) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);
  const std::span<const Bytes> no_messages;

  const DecodeResult result =
      decoder.processPacket(view(moldPacket(21, no_messages)));

  EXPECT_EQ(result.status, DecodeStatus::Ok);
  EXPECT_TRUE(result.had_gap);
  EXPECT_EQ(result.latency_sample_count, 0u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
  EXPECT_EQ(decoder.channelState().heartbeat_next_seq_high_watermark, 21u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
}

TEST(MoldUdpDecoderTest, BuffersOutOfOrderPacketsUntilGapIsFilled) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes msg2 = addMessage(2, 102, 'S', 200, "MSFT", 2000);
  const Bytes msg3 = addMessage(3, 103, 'B', 300, "NVDA", 3000);

  const std::array<Bytes, 4> setup_messages{
      stockDirectoryMessage(1, "AAPL"), stockDirectoryMessage(2, "MSFT"),
      stockDirectoryMessage(3, "NVDA"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const Bytes pkt1 = moldPacket(5, std::span<const Bytes>(&msg1, 1));
  const Bytes pkt3 = moldPacket(7, std::span<const Bytes>(&msg3, 1));
  const Bytes pkt2 = moldPacket(6, std::span<const Bytes>(&msg2, 1));

  const DecodeResult pkt1_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(pkt1_result.status, DecodeStatus::Ok);
  EXPECT_EQ(pkt1_result.latency_sample_count, 1u);
  ASSERT_NE(books.getOrderBook(1), nullptr);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 100u);

  const DecodeResult gap_result = decoder.processPacket(view(pkt3));
  EXPECT_EQ(gap_result.status, DecodeStatus::Ok);
  EXPECT_TRUE(gap_result.had_gap);
  EXPECT_EQ(gap_result.latency_sample_count, 0u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
  const auto gap_stats = decoder.channelState().gap_buffer.stats(6);
  EXPECT_EQ(gap_stats.size, 1u);
  EXPECT_EQ(gap_stats.min_seq, 7u);
  EXPECT_EQ(gap_stats.max_seq, 7u);
  EXPECT_EQ(gap_stats.next_seq_at_or_after, 7u);
  EXPECT_EQ(books.getOrderBook(3)->getTopOfBook().bid_qty, 0u);

  const DecodeResult recovery_result = decoder.processPacket(view(pkt2));
  EXPECT_EQ(recovery_result.status, DecodeStatus::Ok);
  EXPECT_EQ(recovery_result.latency_sample_count, 2u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 8u);

  ASSERT_NE(books.getOrderBook(2), nullptr);
  ASSERT_NE(books.getOrderBook(3), nullptr);
  EXPECT_EQ(books.getOrderBook(2)->getTopOfBook().ask_qty, 200u);
  EXPECT_EQ(books.getOrderBook(3)->getTopOfBook().bid_qty, 300u);
}

TEST(MoldUdpDecoderTest, DuplicatePacketDoesNotContributeToLatency) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const Bytes pkt1 = moldPacket(3, std::span<const Bytes>(&msg1, 1));

  const DecodeResult first_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(first_result.status, DecodeStatus::Ok);
  EXPECT_EQ(first_result.latency_sample_count, 1u);

  const DecodeResult duplicate_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(duplicate_result.status, DecodeStatus::Ok);
  EXPECT_EQ(duplicate_result.latency_sample_count, 0u);
}

TEST(MoldUdpDecoderTest, RejectsHotMessageBeforeDirectoryPreparation) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes message = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes packet = moldPacket(1, std::span<const Bytes>(&message, 1));

  const DecodeResult result = decoder.processPacket(view(packet));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1), nullptr);
}

TEST(MoldUdpDecoderTest,
     LateDirectoryAndPostSystemExecutionSurviveMoldFramingUntilTerminalC) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000)};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, setup_messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::SystemHours);

  // System-event E only advances the lifecycle. A later Stock Directory R
  // still prepares its descriptor, and message-type E remains Order Executed.
  const std::array<Bytes, 3> post_system_messages{
      systemEventMessage('E'), stockDirectoryMessage(2, "MSFT"),
      executeMessage(1, 101, 40)};
  ASSERT_EQ(
      decoder.processPacket(view(moldPacket(4, post_system_messages))).status,
      DecodeStatus::Ok);

  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);
  ASSERT_NE(books.getOrderBook(1), nullptr);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 60u);
  EXPECT_TRUE(symbols.isRegistered(2));
  EXPECT_STREQ(symbols.ticker(2), "MSFT");
  ASSERT_NE(books.getOrderBook(2), nullptr);

  const Bytes terminal = systemEventMessage('C');
  ASSERT_EQ(decoder.processPacket(
                view(moldPacket(7, std::span<const Bytes>(&terminal, 1))))
                .status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::EndOfMessages);

  const Bytes after_terminal = executeMessage(1, 101, 10);
  EXPECT_EQ(decoder.processPacket(
                view(moldPacket(8,
                                std::span<const Bytes>(&after_terminal, 1))))
                .status,
            DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 60u);
}

TEST(MoldUdpDecoderTest,
     EndOfSessionIsAcceptedOnlyAfterTerminalCAndThenRemainsSticky) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 6> messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      systemEventMessage('E'), executeMessage(1, 101, 40),
      systemEventMessage('C')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 7u);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::EndOfMessages);
  ASSERT_EQ(decoder.channelState().status, ChannelHealth::Good);
  ASSERT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 60u);

  const Bytes end = endOfSessionPacket(7);
  EXPECT_EQ(decoder.processPacket(view(end)).status,
            DecodeStatus::EndOfStream);

  const Bytes after_end = addMessage(1, 102, 'B', 10, "AAPL", 900);
  EXPECT_EQ(decoder.processPacket(
                view(moldPacket(7, std::span<const Bytes>(&after_end, 1))))
                .status,
            DecodeStatus::EndOfStream);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 1u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 60u);
}

TEST(MoldUdpDecoderTest, EndOfSessionBeforeTerminalCIsStickyIncomplete) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 5> messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      systemEventMessage('E'), executeMessage(1, 101, 40)};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 6u);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);

  const Bytes premature_end = endOfSessionPacket(6);
  EXPECT_EQ(decoder.processPacket(view(premature_end)).status,
            DecodeStatus::IncompleteSession);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);

  const Bytes terminal = systemEventMessage('C');
  EXPECT_EQ(decoder.processPacket(
                view(moldPacket(6, std::span<const Bytes>(&terminal, 1))))
                .status,
            DecodeStatus::IncompleteSession);
  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 60u);
}

TEST(MoldUdpDecoderTest,
     EndOfSessionRejectsWrongSequenceAndOutstandingHeartbeatGap) {
  {
    astra::symbol::StockDirectory symbols;
    BookManager books;
    MoldUdpDecoder decoder(symbols, books, 3);
    const std::array<Bytes, 3> messages{
        stockDirectoryMessage(1, "AAPL"), systemEventMessage('S'),
        systemEventMessage('C')};
    ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
              DecodeStatus::Ok);
    ASSERT_EQ(decoder.channelState().next_expected_seq, 4u);

    const Bytes wrong_sequence = endOfSessionPacket(5);
    EXPECT_EQ(decoder.processPacket(view(wrong_sequence)).status,
              DecodeStatus::InvalidSequence);
    EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  }

  {
    astra::symbol::StockDirectory symbols;
    BookManager books;
    MoldUdpDecoder decoder(symbols, books, 3);
    const std::array<Bytes, 3> messages{
        stockDirectoryMessage(1, "AAPL"), systemEventMessage('S'),
        systemEventMessage('C')};
    ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
              DecodeStatus::Ok);
    ASSERT_EQ(decoder.channelState().next_expected_seq, 4u);

    const std::span<const Bytes> no_messages;
    ASSERT_EQ(decoder.processPacket(view(moldPacket(10, no_messages))).status,
              DecodeStatus::Ok);
    ASSERT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);

    const Bytes unresolved_end = endOfSessionPacket(4);
    EXPECT_EQ(decoder.processPacket(view(unresolved_end)).status,
              DecodeStatus::InvalidSequence);
    EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  }
}

TEST(MoldUdpDecoderTest, RejectsASecondMoldSessionWithoutPartialReset) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const Bytes add = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes wrong_session =
      moldPacket(3, std::span<const Bytes>(&add, 1), "OTHER     ");
  const DecodeResult result = decoder.processPacket(view(wrong_session));

  EXPECT_EQ(result.status, DecodeStatus::InvalidSession);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);

  const Bytes original_session =
      moldPacket(3, std::span<const Bytes>(&add, 1));
  const DecodeResult after_invalid =
      decoder.processPacket(view(original_session));
  EXPECT_EQ(after_invalid.status, DecodeStatus::InvalidSession);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
}

TEST(MoldUdpDecoderTest, AheadHeartbeatReportsASequenceGap) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const std::span<const Bytes> no_messages;
  const Bytes heartbeat = moldPacket(10, no_messages);
  const DecodeResult result = decoder.processPacket(view(heartbeat));

  EXPECT_EQ(result.status, DecodeStatus::Ok);
  EXPECT_TRUE(result.had_gap);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 3u);
}

TEST(MoldUdpDecoderTest, AheadHeartbeatStaysRecoveringUntilHighWatermark) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 3u);

  const std::span<const Bytes> no_messages;
  const Bytes first_heartbeat = moldPacket(8, no_messages);
  ASSERT_EQ(decoder.processPacket(view(first_heartbeat)).status,
            DecodeStatus::Ok);
  const Bytes later_heartbeat = moldPacket(10, no_messages);
  ASSERT_EQ(decoder.processPacket(view(later_heartbeat)).status,
            DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().heartbeat_next_seq_high_watermark, 10u);

  const std::array<Bytes, 3> first_recovery{
      addMessage(1, 103, 'B', 10, "AAPL", 1000),
      addMessage(1, 104, 'B', 10, "AAPL", 1000),
      addMessage(1, 105, 'B', 10, "AAPL", 1000)};
  const DecodeResult partial =
      decoder.processPacket(view(moldPacket(3, first_recovery)));

  EXPECT_EQ(partial.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 6u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Recovering);
  EXPECT_EQ(decoder.channelState().heartbeat_next_seq_high_watermark, 10u);

  const std::array<Bytes, 4> final_recovery{
      addMessage(1, 106, 'B', 10, "AAPL", 1000),
      addMessage(1, 107, 'B', 10, "AAPL", 1000),
      addMessage(1, 108, 'B', 10, "AAPL", 1000),
      addMessage(1, 109, 'B', 10, "AAPL", 1000)};
  const DecodeResult recovered =
      decoder.processPacket(view(moldPacket(6, final_recovery)));

  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 10u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(decoder.channelState().heartbeat_next_seq_high_watermark, 0u);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 7u);
}

TEST(MoldUdpDecoderTest, BridgePacketDrainsOverlappingBufferedTail) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 3u);

  const std::array<Bytes, 3> ahead{
      addMessage(1, 105, 'B', 10, "AAPL", 1000),
      addMessage(1, 106, 'B', 10, "AAPL", 1000),
      addMessage(1, 107, 'B', 10, "AAPL", 1000)};
  const DecodeResult buffered =
      decoder.processPacket(view(moldPacket(5, ahead)));
  ASSERT_EQ(buffered.status, DecodeStatus::Ok);
  ASSERT_TRUE(buffered.had_gap);
  ASSERT_EQ(decoder.channelState().gap_buffer.size(), 1u);

  // This packet covers [3, 6), including sequence 5 from the buffered packet.
  // Recovery must skip duplicate sequence 5 and apply the buffered [6, 8) tail.
  const std::array<Bytes, 3> bridge{
      addMessage(1, 103, 'B', 10, "AAPL", 1000),
      addMessage(1, 104, 'B', 10, "AAPL", 1000),
      addMessage(1, 105, 'B', 10, "AAPL", 1000)};
  const DecodeResult recovered =
      decoder.processPacket(view(moldPacket(3, bridge)));

  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 8u);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 5u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 50u);
}

TEST(MoldUdpDecoderTest, TruncatedPacketAppliesZeroBookMutations) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const std::array<Bytes, 2> adds{
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      addMessage(1, 102, 'B', 200, "AAPL", 1000)};
  Bytes truncated = moldPacket(3, adds);
  truncated.pop_back();

  const DecodeResult result = decoder.processPacket(view(truncated));

  EXPECT_EQ(result.status, DecodeStatus::InvalidSize);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 0u);
}

TEST(MoldUdpDecoderTest, WrongItchLengthAppliesZeroBookMutations) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 2> setup_messages{
      stockDirectoryMessage(1, "AAPL"), systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  Bytes wrong_length = addMessage(1, 102, 'B', 200, "AAPL", 1000);
  wrong_length.pop_back();
  const std::array<Bytes, 2> messages{
      addMessage(1, 101, 'B', 100, "AAPL", 1000), wrong_length};
  const Bytes packet = moldPacket(3, messages);

  const DecodeResult result = decoder.processPacket(view(packet));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 0u);
}

TEST(MoldUdpDecoderTest, HeartbeatWithTrailingPayloadIsRejected) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::span<const Bytes> no_messages;
  Bytes heartbeat = moldPacket(1, no_messages);
  appendU8(heartbeat, 0xAB);

  const DecodeResult result = decoder.processPacket(view(heartbeat));

  EXPECT_EQ(result.status, DecodeStatus::InvalidSize);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
}
