#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelHealth.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/core/Time.hpp"
#include "astra/protocol/SymbolTable.hpp"
#include "astra/source/PacketView.hpp"

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

Bytes moldPacket(uint64_t first_seq, std::span<const Bytes> messages) {
  Bytes b;
  constexpr char kSession[10] = {'A', 'S', 'T', 'R', 'A',
                                 ' ', ' ', ' ', ' ', ' '};
  for (char c : kSession) appendU8(b, static_cast<uint8_t>(c));
  appendU64(b, first_seq);
  appendU16(b, static_cast<uint16_t>(messages.size()));
  for (const Bytes &message : messages) {
    appendU16(b, static_cast<uint16_t>(message.size()));
    b.insert(b.end(), message.begin(), message.end());
  }
  return b;
}

PacketView view(const Bytes &b) {
  return {b.data(), b.size(), rdtsc()};
}

} // namespace

TEST(MoldUdpDecoderTest, BuffersOutOfOrderPacketsUntilGapIsFilled) {
  SymbolTable symbols;
  BookManager books;
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
  EXPECT_EQ(books.getOrderBook(3), nullptr);

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
