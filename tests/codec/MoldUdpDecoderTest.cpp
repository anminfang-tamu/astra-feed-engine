#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelHealth.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/source/PacketView.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;
constexpr size_t kTestOrderCapacity = 1024;
constexpr PriceLevelArenaConfig kTestPriceLevelConfig{256, 128, 128};

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
  for (std::size_t i = 0; i < ChannelState::kSessionBytes; ++i) {
    appendU8(b, i < session.size() ? static_cast<uint8_t>(session[i]) : ' ');
  }
  appendU64(b, first_seq);
  appendU16(b, static_cast<uint16_t>(messages.size()));
  for (const Bytes &message : messages) {
    appendU16(b, static_cast<uint16_t>(message.size()));
    b.insert(b.end(), message.begin(), message.end());
  }
  return b;
}

Bytes moldControlPacket(uint64_t first_seq, uint16_t message_count,
                        std::string_view session = "ASTRA     ") {
  Bytes b;
  for (std::size_t i = 0; i < ChannelState::kSessionBytes; ++i) {
    appendU8(b, i < session.size() ? static_cast<uint8_t>(session[i]) : ' ');
  }
  appendU64(b, first_seq);
  appendU16(b, message_count);
  return b;
}

PacketView view(const Bytes &b, uint8_t line_index = 0) {
  return {b.data(), b.size(), rdtsc(), line_index};
}

void registerSymbol(astra::symbol::StockDirectory &symbols, uint16_t locate,
                    const char *ticker) {
  astra::symbol::StockDirectoryEntry entry{};
  std::size_t i = 0;
  while (ticker[i] != '\0' && i < 8) {
    entry.ticker[i] = ticker[i];
    ++i;
  }
  entry.ticker[i] = '\0';
  entry.locate = locate;
  symbols.set(locate, entry);
}

bool prepareRegisteredBooks(const astra::symbol::StockDirectory &symbols,
                            BookManager &books) {
  bool found_registered_symbol = false;
  for (uint16_t locate = 1;
       locate < astra::symbol::StockDirectory::kMaxLocate; ++locate) {
    if (!symbols.isRegistered(locate)) {
      continue;
    }
    found_registered_symbol = true;
    if (!books.prepareBook(locate)) {
      return false;
    }
  }
  if (!found_registered_symbol) {
    return false;
  }
  books.sealBookUniverse();
  return true;
}

} // namespace

TEST(MoldUdpDecoderTest, ParserReadinessFailureInvalidatesChannel) {
  astra::symbol::StockDirectory symbols;
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  MoldUdpDecoder decoder(symbols, books, 3);
  const Bytes market_hours = systemEventMessage('S');
  const Bytes packet =
      moldPacket(1, std::span<const Bytes>(&market_hours, 1));

  const DecodeResult result = decoder.processPacket(view(packet));

  EXPECT_EQ(result.status, DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Invalid);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
  EXPECT_FALSE(decoder.channelState().session_initialized);

  const Bytes add = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes retry = moldPacket(1, std::span<const Bytes>(&add, 1));
  EXPECT_EQ(decoder.processPacket(view(retry)).status,
            DecodeStatus::InvalidItchMessage);
  EXPECT_EQ(books.getOrderBook(1), nullptr);
}

TEST(MoldUdpDecoderTest, BuffersOutOfOrderPacketsUntilGapIsFilled) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  registerSymbol(symbols, 2, "MSFT");
  registerSymbol(symbols, 3, "NVDA");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes msg2 = addMessage(2, 102, 'S', 200, "MSFT", 2000);
  const Bytes msg3 = addMessage(3, 103, 'B', 300, "NVDA", 3000);

  const Bytes pkt1 = moldPacket(1, std::span<const Bytes>(&msg1, 1));
  const Bytes pkt3 = moldPacket(3, std::span<const Bytes>(&msg3, 1));
  const Bytes pkt2 = moldPacket(2, std::span<const Bytes>(&msg2, 1));

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
  const auto gap_stats = decoder.channelState().gap_buffer.stats(2);
  EXPECT_EQ(gap_stats.size, 1u);
  EXPECT_EQ(gap_stats.min_seq, 3u);
  EXPECT_EQ(gap_stats.max_seq, 3u);
  EXPECT_EQ(gap_stats.next_seq_at_or_after, 3u);
  ASSERT_NE(books.getOrderBook(3), nullptr);
  EXPECT_EQ(books.getOrderBook(3)->liveOrderCount(), 0u);

  const DecodeResult recovery_result = decoder.processPacket(view(pkt2));
  EXPECT_EQ(recovery_result.status, DecodeStatus::Ok);
  EXPECT_EQ(recovery_result.latency_sample_count, 2u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 4u);

  ASSERT_NE(books.getOrderBook(2), nullptr);
  ASSERT_NE(books.getOrderBook(3), nullptr);
  EXPECT_EQ(books.getOrderBook(2)->getTopOfBook().ask_qty, 200u);
  EXPECT_EQ(books.getOrderBook(3)->getTopOfBook().bid_qty, 300u);
}

TEST(MoldUdpDecoderTest, DuplicatePacketDoesNotContributeToLatency) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes pkt1 = moldPacket(1, std::span<const Bytes>(&msg1, 1));

  const DecodeResult first_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(first_result.status, DecodeStatus::Ok);
  EXPECT_EQ(first_result.latency_sample_count, 1u);

  const DecodeResult duplicate_result = decoder.processPacket(view(pkt1));
  EXPECT_EQ(duplicate_result.status, DecodeStatus::Ok);
  EXPECT_EQ(duplicate_result.latency_sample_count, 0u);
}

TEST(MoldUdpDecoderTest, FirstFuturePacketWaitsForSequenceOneAndThenDrains) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  registerSymbol(symbols, 2, "MSFT");
  registerSymbol(symbols, 3, "NVDA");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes msg2 = addMessage(2, 102, 'S', 200, "MSFT", 2000);
  const Bytes msg3 = addMessage(3, 103, 'B', 300, "NVDA", 3000);
  const Bytes first_messages[] = {msg1, msg2};

  const Bytes future = moldPacket(3, std::span<const Bytes>(&msg3, 1));
  const Bytes stream_head = moldPacket(1, first_messages);

  const DecodeResult future_result = decoder.processPacket(view(future, 1));
  EXPECT_EQ(future_result.status, DecodeStatus::Ok);
  EXPECT_TRUE(future_result.had_gap);
  EXPECT_EQ(future_result.latency_sample_count, 0u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::GapDetected);
  EXPECT_FALSE(decoder.channelState().session_initialized);
  EXPECT_EQ(decoder.channelState().first_received_seq, 3u);
  EXPECT_EQ(decoder.channelState().first_received_seq_by_line[0], 0u);
  EXPECT_EQ(decoder.channelState().first_received_seq_by_line[1], 3u);
  EXPECT_EQ(decoder.channelState().gap_buffer.size(), 1u);
  ASSERT_NE(books.getOrderBook(3), nullptr);
  EXPECT_EQ(books.getOrderBook(3)->liveOrderCount(), 0u);

  const DecodeResult recovered = decoder.processPacket(view(stream_head, 0));
  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_EQ(recovered.latency_sample_count, 3u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 4u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  EXPECT_TRUE(decoder.channelState().session_initialized);
  EXPECT_EQ(decoder.channelState().first_received_seq, 3u);
  EXPECT_EQ(decoder.channelState().first_received_seq_by_line[0], 1u);
  EXPECT_EQ(decoder.channelState().first_received_seq_by_line[1], 3u);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());

  ASSERT_NE(books.getOrderBook(1), nullptr);
  ASSERT_NE(books.getOrderBook(2), nullptr);
  ASSERT_NE(books.getOrderBook(3), nullptr);
}

TEST(MoldUdpDecoderTest, SessionMismatchDoesNotAdvanceSequence) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  registerSymbol(symbols, 2, "MSFT");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes msg2 = addMessage(2, 102, 'S', 200, "MSFT", 2000);
  const Bytes pkt1 =
      moldPacket(1, std::span<const Bytes>(&msg1, 1), "SESSION-A ");
  const Bytes wrong_session =
      moldPacket(2, std::span<const Bytes>(&msg2, 1), "SESSION-B ");
  const Bytes matching_session =
      moldPacket(2, std::span<const Bytes>(&msg2, 1), "SESSION-A ");

  EXPECT_EQ(decoder.processPacket(view(pkt1)).status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 2u);

  const DecodeResult mismatch = decoder.processPacket(view(wrong_session, 1));
  EXPECT_EQ(mismatch.status, DecodeStatus::InvalidSequence);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 2u);
  EXPECT_EQ(decoder.channelState().session_mismatch_packets, 1u);
  ASSERT_NE(books.getOrderBook(2), nullptr);
  EXPECT_EQ(books.getOrderBook(2)->liveOrderCount(), 0u);

  const DecodeResult recovered =
      decoder.processPacket(view(matching_session, 0));
  EXPECT_EQ(recovered.status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 3u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
  ASSERT_NE(books.getOrderBook(2), nullptr);
}

TEST(MoldUdpDecoderTest, MalformedStreamHeadDoesNotBindSession) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  Bytes malformed =
      moldPacket(1, std::span<const Bytes>(&msg1, 1), "SESSION-A ");
  malformed.pop_back();
  const Bytes valid =
      moldPacket(1, std::span<const Bytes>(&msg1, 1), "SESSION-B ");

  EXPECT_EQ(decoder.processPacket(view(malformed)).status,
            DecodeStatus::InvalidSize);
  EXPECT_FALSE(decoder.channelState().session_initialized);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);

  EXPECT_EQ(decoder.processPacket(view(valid, 1)).status, DecodeStatus::Ok);
  EXPECT_TRUE(decoder.channelState().session_initialized);
  EXPECT_TRUE(decoder.channelState().sessionMatches("SESSION-B "));
  EXPECT_EQ(decoder.channelState().next_expected_seq, 2u);
}

TEST(MoldUdpDecoderTest, RejectsSequenceRangeOverflow) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);
  decoder.channelState().next_expected_seq =
      std::numeric_limits<uint64_t>::max();

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes packet = moldPacket(std::numeric_limits<uint64_t>::max(),
                                  std::span<const Bytes>(&msg1, 1));

  EXPECT_EQ(decoder.processPacket(view(packet)).status,
            DecodeStatus::InvalidSequence);
  EXPECT_EQ(decoder.channelState().next_expected_seq,
            std::numeric_limits<uint64_t>::max());
  EXPECT_FALSE(decoder.channelState().session_initialized);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());
}

TEST(MoldUdpDecoderTest, FutureEndOfSessionDoesNotStopColdDecoder) {
  astra::symbol::StockDirectory symbols;
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes future_end = moldControlPacket(
      4401, MoldUdpPacketHeader::kEndOfSessionMessageCount);

  EXPECT_EQ(decoder.processPacket(view(future_end, 1)).status,
            DecodeStatus::InvalidSequence);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 1u);
  EXPECT_FALSE(decoder.channelState().session_initialized);
}

TEST(MoldUdpDecoderTest, RedundantFutureCopyKeepsEarliestReceiveTimestamp) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes packet = moldPacket(3, std::span<const Bytes>(&msg, 1));
  const PacketView line_a{packet.data(), packet.size(), 111, 0};
  const PacketView line_b{packet.data(), packet.size(), 222, 1};

  EXPECT_EQ(decoder.processPacket(line_a).status, DecodeStatus::Ok);
  EXPECT_EQ(decoder.processPacket(line_b).status, DecodeStatus::Ok);

  const SequencedPacket *buffered = decoder.channelState().gap_buffer.find(3);
  ASSERT_NE(buffered, nullptr);
  EXPECT_EQ(buffered->receive_start_ticks, 111u);
  EXPECT_EQ(decoder.channelState().gap_buffer.size(), 1u);
  EXPECT_EQ(decoder.channelState().first_received_seq_by_line[0], 3u);
  EXPECT_EQ(decoder.channelState().first_received_seq_by_line[1], 3u);
}

TEST(MoldUdpDecoderTest,
     BufferedSessionMismatchPreservesRecoveredPacketLatency) {
  astra::symbol::StockDirectory symbols;
  registerSymbol(symbols, 1, "AAPL");
  registerSymbol(symbols, 2, "MSFT");
  registerSymbol(symbols, 3, "NVDA");
  BookManager books(kTestOrderCapacity, kTestPriceLevelConfig);
  ASSERT_TRUE(prepareRegisteredBooks(symbols, books));
  MoldUdpDecoder decoder(symbols, books, 3);

  const Bytes msg1 = addMessage(1, 101, 'B', 100, "AAPL", 1000);
  const Bytes msg2 = addMessage(2, 102, 'S', 200, "MSFT", 2000);
  const Bytes msg3 = addMessage(3, 103, 'B', 300, "NVDA", 3000);
  const Bytes head_messages[] = {msg1, msg2};
  const Bytes stale_future =
      moldPacket(3, std::span<const Bytes>(&msg3, 1), "SESSION-A ");
  const Bytes valid_head = moldPacket(1, head_messages, "SESSION-B ");
  const Bytes valid_future =
      moldPacket(3, std::span<const Bytes>(&msg3, 1), "SESSION-B ");

  EXPECT_EQ(decoder.processPacket(view(stale_future, 1)).status,
            DecodeStatus::Ok);
  const DecodeResult head_result = decoder.processPacket(view(valid_head, 0));
  EXPECT_EQ(head_result.status, DecodeStatus::Ok);
  EXPECT_EQ(head_result.latency_sample_count, 2u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 3u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Recovering);
  EXPECT_EQ(decoder.channelState().session_mismatch_packets, 1u);
  EXPECT_TRUE(decoder.channelState().gap_buffer.empty());

  const DecodeResult future_result =
      decoder.processPacket(view(valid_future, 0));
  EXPECT_EQ(future_result.status, DecodeStatus::Ok);
  EXPECT_EQ(future_result.latency_sample_count, 1u);
  EXPECT_EQ(decoder.channelState().next_expected_seq, 4u);
  EXPECT_EQ(decoder.channelState().status, ChannelHealth::Good);
}
