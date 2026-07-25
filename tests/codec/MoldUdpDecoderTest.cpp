#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelHealth.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/protocol/ItchPrice.hpp"
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

Bytes cancelMessage(uint16_t locate, uint64_t order_id,
                    uint32_t canceled_qty) {
  Bytes b;
  appendU8(b, 'X');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendU64(b, order_id);
  appendU32(b, canceled_qty);
  return b;
}

Bytes deleteMessage(uint16_t locate, uint64_t order_id) {
  Bytes b;
  appendU8(b, 'D');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendU64(b, order_id);
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

Bytes systemEventMessage(char event_code, uint16_t locate = 0) {
  Bytes b;
  appendU8(b, 'S');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendU8(b, static_cast<uint8_t>(event_code));
  return b;
}

Bytes directListingMessage(uint16_t locate, std::string_view sym,
                           char eligibility = 'Y') {
  Bytes b;
  appendU8(b, 'O');
  appendU16(b, locate);
  appendU16(b, 7);
  appendU48(b, 34200123456789ULL);
  appendStock(b, sym);
  appendU8(b, static_cast<uint8_t>(eligibility));
  appendU32(b, 800'000);
  appendU32(b, 1'800'000);
  appendU32(b, 1'250'000);
  appendU64(b, 34200123456789ULL);
  appendU32(b, 1'125'000);
  appendU32(b, 1'375'000);
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

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const DecodeResult setup =
      decoder.processPacket(view(moldPacket(1, setup_messages)));

  EXPECT_EQ(setup.status, DecodeStatus::Ok);
  EXPECT_EQ(setup.latency_sample_count, 3u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 4u);
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

  const std::array<Bytes, 5> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      stockDirectoryMessage(2, "MSFT"), stockDirectoryMessage(3, "NVDA"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const Bytes pkt1 = moldPacket(6, std::span<const Bytes>(&msg1, 1));
  const Bytes pkt3 = moldPacket(8, std::span<const Bytes>(&msg3, 1));
  const Bytes pkt2 = moldPacket(7, std::span<const Bytes>(&msg2, 1));

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
  const auto gap_stats = decoder.channelState().gap_buffer.stats(7);
  EXPECT_EQ(gap_stats.size, 1u);
  EXPECT_EQ(gap_stats.min_seq, 8u);
  EXPECT_EQ(gap_stats.max_seq, 8u);
  EXPECT_EQ(gap_stats.next_seq_at_or_after, 8u);
  EXPECT_EQ(books.getOrderBook(3)->getTopOfBook().bid_qty, 0u);

  const DecodeResult recovery_result = decoder.processPacket(view(pkt2));
  EXPECT_EQ(recovery_result.status, DecodeStatus::Ok);
  EXPECT_EQ(recovery_result.latency_sample_count, 2u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 9u);

  ASSERT_NE(books.getOrderBook(2), nullptr);
  ASSERT_NE(books.getOrderBook(3), nullptr);
  EXPECT_EQ(books.getOrderBook(2)->getTopOfBook().ask_qty, 200u);
  EXPECT_EQ(books.getOrderBook(3)->getTopOfBook().bid_qty, 300u);
}

TEST(MoldUdpDecoderTest,
     ConflictingBufferedRedundantPacketIsTerminalAndCannotOverwriteFirst) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes start = systemEventMessage('O');
  ASSERT_EQ(decoder.processPacket(
                view(moldPacket(1, std::span<const Bytes>(&start, 1))))
                .status,
            DecodeStatus::Ok);

  const Bytes a_directory = stockDirectoryMessage(1, "AAPL");
  const Bytes b_directory = stockDirectoryMessage(1, "MSFT");
  const Bytes line_a =
      moldPacket(3, std::span<const Bytes>(&a_directory, 1));
  const Bytes identical_line_b =
      moldPacket(3, std::span<const Bytes>(&a_directory, 1));
  const Bytes conflicting_line_b =
      moldPacket(3, std::span<const Bytes>(&b_directory, 1));

  ASSERT_EQ(decoder.processPacket(view(line_a)).status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().gap_buffer.size(), 1u);
  EXPECT_EQ(decoder.processPacket(view(identical_line_b)).status,
            DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().gap_buffer.size(), 1u);
  EXPECT_EQ(
      decoder.channelState().identical_buffered_redundant_packets, 1u);

  EXPECT_EQ(decoder.processPacket(view(conflicting_line_b)).status,
            DecodeStatus::RedundantFeedMismatch);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(
      decoder.channelState().conflicting_buffered_redundant_packets, 1u);
  EXPECT_EQ(decoder.channelState().gap_buffer.size(), 1u);
  EXPECT_EQ(symbols.size(), 0u);
}

TEST(MoldUdpDecoderTest, DuplicatePacketDoesNotContributeToLatency) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const Bytes pkt1 = moldPacket(4, std::span<const Bytes>(&msg1, 1));

  const DecodeResult first_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(first_result.status, DecodeStatus::Ok);
  EXPECT_EQ(first_result.latency_sample_count, 1u);

  const DecodeResult duplicate_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(duplicate_result.status, DecodeStatus::Ok);
  EXPECT_EQ(duplicate_result.latency_sample_count, 0u);
}

TEST(MoldUdpDecoderTest,
     DirectListingPriceDiscoveryAdvancesSequenceWithoutMutatingBook) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes direct_listing = directListingMessage(1, "DLCR");
  ASSERT_EQ(direct_listing.size(), 48u);
  const std::array<Bytes, 5> messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "DLCR"),
      systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "DLCR", 1'200'000), direct_listing};

  const DecodeResult result =
      decoder.processPacket(view(moldPacket(1, messages)));

  EXPECT_EQ(result.status, DecodeStatus::Ok);
  EXPECT_EQ(result.latency_sample_count, 5u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 6u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::SystemHours);
  ASSERT_NE(books.getOrderBook(1), nullptr);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 1u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_price, 1'200'000u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 100u);
}

TEST(MoldUdpDecoderTest,
     WrongDirectListingLengthRejectsWholePacketBeforeBookMutation) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "DLCR"),
      systemEventMessage('S')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, setup_messages))).status,
            DecodeStatus::Ok);

  Bytes truncated = directListingMessage(1, "DLCR");
  ASSERT_EQ(truncated.size(), 48u);
  truncated.pop_back();
  const std::array<Bytes, 2> messages{
      addMessage(1, 101, 'B', 100, "DLCR", 1'200'000), truncated};

  const DecodeResult result =
      decoder.processPacket(view(moldPacket(4, messages)));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 0u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
}

TEST(MoldUdpDecoderTest,
     PriceAboveNasdaqPrice4MaximumIsTerminalWithoutBookMutation) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, setup_messages))).status,
            DecodeStatus::Ok);

  const Bytes invalid_add =
      addMessage(1, 101, 'B', 100, "AAPL",
                 astra::protocol::kMaxItchPrice4 + 1u);
  const DecodeResult result = decoder.processPacket(
      view(moldPacket(4, std::span<const Bytes>(&invalid_add, 1))));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 0u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
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
     FirstStockDirectoryIsTerminalWithoutDirectoryOrBookMutation) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes directory = stockDirectoryMessage(1, "AAPL");
  const DecodeResult result = decoder.processPacket(
      view(moldPacket(1, std::span<const Bytes>(&directory, 1))));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(decoder.channelState().phase,
            ChannelPhase::WaitingStartOfMessages);
  EXPECT_EQ(symbols.size(), 0u);
  EXPECT_EQ(books.getOrderBook(1), nullptr);
}

TEST(MoldUdpDecoderTest,
     FirstSystemHoursEventIsTerminalWithoutDirectoryOrBookMutation) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes system_hours = systemEventMessage('S');
  const DecodeResult result = decoder.processPacket(
      view(moldPacket(1, std::span<const Bytes>(&system_hours, 1))));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(decoder.channelState().phase,
            ChannelPhase::WaitingStartOfMessages);
  EXPECT_EQ(symbols.size(), 0u);
  EXPECT_EQ(books.getOrderBook(1), nullptr);
}

TEST(MoldUdpDecoderTest,
     NonzeroSystemEventStockLocateIsTerminalWithoutPhaseChange) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes invalid_start =
      systemEventMessage('O', /*locate=*/7);
  const DecodeResult result = decoder.processPacket(
      view(moldPacket(1, std::span<const Bytes>(&invalid_start, 1))));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(decoder.channelState().phase,
            ChannelPhase::WaitingStartOfMessages);
  EXPECT_EQ(symbols.size(), 0u);
  EXPECT_EQ(books.totalLiveOrderCount(), 0u);
}

TEST(MoldUdpDecoderTest,
     LateDirectoryAndPostSystemCancelDeleteTailSurviveUntilTerminalC) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 5> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      addMessage(1, 102, 'S', 50, "AAPL", 1100)};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, setup_messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::SystemHours);

  const std::array<Bytes, 7> post_system_messages{
      stockDirectoryMessage(2, "MSFT"), systemEventMessage('Q'),
      systemEventMessage('M'), systemEventMessage('E'),
      deleteMessage(1, 102),
      cancelMessage(1, 101, 25),
      deleteMessage(1, 101)};
  ASSERT_EQ(
      decoder.processPacket(view(moldPacket(6, post_system_messages))).status,
      DecodeStatus::Ok);

  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);
  ASSERT_NE(books.getOrderBook(1), nullptr);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
  EXPECT_TRUE(symbols.isRegistered(2));
  EXPECT_STREQ(symbols.ticker(2), "MSFT");
  ASSERT_NE(books.getOrderBook(2), nullptr);

  const Bytes terminal = systemEventMessage('C');
  ASSERT_EQ(decoder.processPacket(
                view(moldPacket(13, std::span<const Bytes>(&terminal, 1))))
                .status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::EndOfMessages);

  const Bytes after_terminal =
      addMessage(1, 102, 'B', 10, "AAPL", 900);
  EXPECT_EQ(decoder.processPacket(
                view(moldPacket(14,
                                std::span<const Bytes>(&after_terminal, 1))))
                .status,
            DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
}

TEST(MoldUdpDecoderTest,
     AddAfterSystemEventEIsTerminalWithoutApplyingTheAdd) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 8> messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      systemEventMessage('Q'), systemEventMessage('M'),
      systemEventMessage('E'),
      addMessage(1, 102, 'B', 25, "AAPL", 900)};

  const DecodeResult result =
      decoder.processPacket(view(moldPacket(1, messages)));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);
  ASSERT_NE(books.getOrderBook(1), nullptr);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 1u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_price, 1000u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 100u);
}

TEST(MoldUdpDecoderTest,
     EndOfSessionIsAcceptedOnlyAfterTerminalCAndThenRemainsSticky) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 9> messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      systemEventMessage('Q'), systemEventMessage('M'),
      systemEventMessage('E'), deleteMessage(1, 101),
      systemEventMessage('C')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 10u);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::EndOfMessages);
  ASSERT_EQ(decoder.channelState().status, ChannelHealth::Good);
  ASSERT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);

  const Bytes end = endOfSessionPacket(10);
  EXPECT_EQ(decoder.processPacket(view(end)).status,
            DecodeStatus::EndOfStream);
  EXPECT_TRUE(decoder.endOfStreamAccepted());
  EXPECT_EQ(decoder.terminalStatus(), DecodeStatus::EndOfStream);

  const Bytes after_end = addMessage(1, 102, 'B', 10, "AAPL", 900);
  EXPECT_EQ(decoder.processPacket(
                view(moldPacket(10, std::span<const Bytes>(&after_end, 1))))
                .status,
            DecodeStatus::EndOfStream);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 0u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
}

TEST(MoldUdpDecoderTest,
     AheadEndOfSessionWaitsForRedundantGapRecovery) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, setup_messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 4u);

  const std::array<Bytes, 3> ahead_tail{
      systemEventMessage('M'), systemEventMessage('E'),
      systemEventMessage('C')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(5, ahead_tail))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
  ASSERT_EQ(decoder.channelState().gap_buffer.size(), 1u);

  const Bytes end = endOfSessionPacket(8);
  const DecodeResult pending_end = decoder.processPacket(view(end));
  EXPECT_EQ(pending_end.status, DecodeStatus::Ok);
  EXPECT_TRUE(pending_end.had_gap);
  EXPECT_FALSE(decoder.endOfStreamAccepted());

  // A repeated EOS from the other line is the same pending boundary, not a
  // contradiction.
  EXPECT_EQ(decoder.processPacket(view(end)).status, DecodeStatus::Ok);

  // The missing Q packet arrives from the redundant line. Processing it also
  // drains M/E/C from the gap buffer and accepts the retained EOS boundary.
  const Bytes missing = systemEventMessage('Q');
  const DecodeResult recovered = decoder.processPacket(
      view(moldPacket(4, std::span<const Bytes>(&missing, 1))));
  EXPECT_EQ(recovered.status, DecodeStatus::EndOfStream);
  EXPECT_TRUE(recovered.had_gap);
  EXPECT_TRUE(decoder.endOfStreamAccepted());
  EXPECT_EQ(decoder.channelState().next_expected_seq, 8u);
  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::EndOfMessages);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());
}

TEST(MoldUdpDecoderTest, SystemEventCWithLiveOrdersIsTerminalInvalid) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 8> messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      systemEventMessage('Q'), systemEventMessage('M'),
      systemEventMessage('E'), systemEventMessage('C')};

  const DecodeResult result =
      decoder.processPacket(view(moldPacket(1, messages)));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);
  EXPECT_EQ(books.totalLiveOrderCount(), 1u);
}

TEST(MoldUdpDecoderTest, EndOfSessionBeforeTerminalCIsStickyIncomplete) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 7> messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S'),
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      systemEventMessage('Q'), systemEventMessage('M'),
      systemEventMessage('E')};
  ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
            DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 8u);
  ASSERT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);

  const Bytes premature_end = endOfSessionPacket(8);
  EXPECT_EQ(decoder.processPacket(view(premature_end)).status,
            DecodeStatus::IncompleteSession);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);

  const Bytes terminal = systemEventMessage('C');
  EXPECT_EQ(decoder.processPacket(
                view(moldPacket(8, std::span<const Bytes>(&terminal, 1))))
                .status,
            DecodeStatus::IncompleteSession);
  EXPECT_EQ(decoder.channelState().phase, ChannelPhase::PostSystemHours);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 100u);
}

TEST(MoldUdpDecoderTest,
     EndOfSessionRejectsWrongSequenceAndOutstandingHeartbeatGap) {
  {
    astra::symbol::StockDirectory symbols;
    BookManager books;
    MoldUdpDecoder decoder(symbols, books, 3);
    const std::array<Bytes, 7> messages{
        systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
        systemEventMessage('S'), systemEventMessage('Q'),
        systemEventMessage('M'), systemEventMessage('E'),
        systemEventMessage('C')};
    ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
              DecodeStatus::Ok);
    ASSERT_EQ(decoder.channelState().next_expected_seq, 8u);

    const Bytes wrong_sequence = endOfSessionPacket(9);
    EXPECT_EQ(decoder.processPacket(view(wrong_sequence)).status,
              DecodeStatus::InvalidSequence);
    EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  }

  {
    astra::symbol::StockDirectory symbols;
    BookManager books;
    MoldUdpDecoder decoder(symbols, books, 3);
    const std::array<Bytes, 7> messages{
        systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
        systemEventMessage('S'), systemEventMessage('Q'),
        systemEventMessage('M'), systemEventMessage('E'),
        systemEventMessage('C')};
    ASSERT_EQ(decoder.processPacket(view(moldPacket(1, messages))).status,
              DecodeStatus::Ok);
    ASSERT_EQ(decoder.channelState().next_expected_seq, 8u);

    const std::span<const Bytes> no_messages;
    ASSERT_EQ(decoder.processPacket(view(moldPacket(10, no_messages))).status,
              DecodeStatus::Ok);
    ASSERT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);

    const Bytes unresolved_end = endOfSessionPacket(8);
    EXPECT_EQ(decoder.processPacket(view(unresolved_end)).status,
              DecodeStatus::InvalidSequence);
    EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  }
}

TEST(MoldUdpDecoderTest, RejectsASecondMoldSessionWithoutPartialReset) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const Bytes add = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes wrong_session =
      moldPacket(4, std::span<const Bytes>(&add, 1), "OTHER     ");
  const DecodeResult result = decoder.processPacket(view(wrong_session));

  EXPECT_EQ(result.status, DecodeStatus::InvalidSession);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);

  const Bytes original_session =
      moldPacket(4, std::span<const Bytes>(&add, 1));
  const DecodeResult after_invalid =
      decoder.processPacket(view(original_session));
  EXPECT_EQ(after_invalid.status, DecodeStatus::InvalidSession);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 0u);
}

TEST(MoldUdpDecoderTest, AheadHeartbeatReportsASequenceGap) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const std::span<const Bytes> no_messages;
  const Bytes heartbeat = moldPacket(10, no_messages);
  const DecodeResult result = decoder.processPacket(view(heartbeat));

  EXPECT_EQ(result.status, DecodeStatus::Ok);
  EXPECT_TRUE(result.had_gap);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 4u);
}

TEST(MoldUdpDecoderTest, AheadHeartbeatStaysRecoveringUntilHighWatermark) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 4u);

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
      decoder.processPacket(view(moldPacket(4, first_recovery)));

  EXPECT_EQ(partial.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 7u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Recovering);
  EXPECT_EQ(decoder.channelState().heartbeat_next_seq_high_watermark, 10u);

  const std::array<Bytes, 4> final_recovery{
      addMessage(1, 106, 'B', 10, "AAPL", 1000),
      addMessage(1, 107, 'B', 10, "AAPL", 1000),
      addMessage(1, 108, 'B', 10, "AAPL", 1000),
      addMessage(1, 109, 'B', 10, "AAPL", 1000)};
  const DecodeResult recovered =
      decoder.processPacket(view(moldPacket(7, final_recovery)));

  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 11u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(decoder.channelState().heartbeat_next_seq_high_watermark, 0u);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 7u);
}

TEST(MoldUdpDecoderTest, BridgePacketDrainsOverlappingBufferedTail) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);
  ASSERT_EQ(decoder.channelState().next_expected_seq, 4u);

  const std::array<Bytes, 3> ahead{
      addMessage(1, 105, 'B', 10, "AAPL", 1000),
      addMessage(1, 106, 'B', 10, "AAPL", 1000),
      addMessage(1, 107, 'B', 10, "AAPL", 1000)};
  const DecodeResult buffered =
      decoder.processPacket(view(moldPacket(6, ahead)));
  ASSERT_EQ(buffered.status, DecodeStatus::Ok);
  ASSERT_TRUE(buffered.had_gap);
  ASSERT_EQ(decoder.channelState().gap_buffer.size(), 1u);

  // This packet covers [4, 7), including sequence 6 from the buffered packet.
  // Recovery must skip duplicate sequence 6 and apply the buffered [7, 9) tail.
  const std::array<Bytes, 3> bridge{
      addMessage(1, 103, 'B', 10, "AAPL", 1000),
      addMessage(1, 104, 'B', 10, "AAPL", 1000),
      addMessage(1, 105, 'B', 10, "AAPL", 1000)};
  const DecodeResult recovered =
      decoder.processPacket(view(moldPacket(4, bridge)));

  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 9u);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(books.getOrderBook(1)->liveOrderCount(), 5u);
  EXPECT_EQ(books.getOrderBook(1)->getTopOfBook().bid_qty, 50u);
}

TEST(MoldUdpDecoderTest, TruncatedPacketAppliesZeroBookMutations) {
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books, 3);

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  const std::array<Bytes, 2> adds{
      addMessage(1, 101, 'B', 100, "AAPL", 1000),
      addMessage(1, 102, 'B', 200, "AAPL", 1000)};
  Bytes truncated = moldPacket(4, adds);
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

  const std::array<Bytes, 3> setup_messages{
      systemEventMessage('O'), stockDirectoryMessage(1, "AAPL"),
      systemEventMessage('S')};
  const Bytes setup = moldPacket(1, setup_messages);
  ASSERT_EQ(decoder.processPacket(view(setup)).status, DecodeStatus::Ok);

  Bytes wrong_length = addMessage(1, 102, 'B', 200, "AAPL", 1000);
  wrong_length.pop_back();
  const std::array<Bytes, 2> messages{
      addMessage(1, 101, 'B', 100, "AAPL", 1000), wrong_length};
  const Bytes packet = moldPacket(4, messages);

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
