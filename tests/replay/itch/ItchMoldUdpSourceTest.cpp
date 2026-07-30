#include "replay/itch/ItchMoldUdpSource.hpp"

#include "astra/book/BookManager.hpp"
#include "astra/book/BookUpdate.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/protocol/ItchPrice.hpp"
#include "astra/protocol/MoldUdpControlPacket.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using CompletionKind = ItchMoldUdpSource::CompletionKind;
using CompletionPolicy = ItchMoldUdpSource::CompletionPolicy;

class BinaryFile {
public:
  explicit BinaryFile(const std::vector<std::uint8_t> &bytes) {
    const auto *test = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::temp_directory_path() /
            ("astra-itch-source-" + std::to_string(::getpid()) + "-" +
             test->name() + ".bin");
    std::ofstream output(path_, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
  }

  ~BinaryFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

void appendRecord(std::vector<std::uint8_t> &file,
                  const std::vector<std::uint8_t> &message) {
  ASSERT_LE(message.size(), 0xFFFFu);
  const auto size = static_cast<std::uint16_t>(message.size());
  file.push_back(static_cast<std::uint8_t>(size >> 8));
  file.push_back(static_cast<std::uint8_t>(size));
  file.insert(file.end(), message.begin(), message.end());
}

void appendTerminator(std::vector<std::uint8_t> &file) {
  file.push_back(0);
  file.push_back(0);
}

std::vector<std::uint8_t> systemEvent(char code,
                                      std::uint16_t locate = 0) {
  std::vector<std::uint8_t> message(12, 0);
  message[0] = 'S';
  message[1] = static_cast<std::uint8_t>(locate >> 8);
  message[2] = static_cast<std::uint8_t>(locate);
  message[11] = static_cast<std::uint8_t>(code);
  return message;
}

std::vector<std::uint8_t> directListing() {
  std::vector<std::uint8_t> message(48, 0);
  message[0] = 'O';
  return message;
}

std::vector<std::uint8_t> noii() {
  std::vector<std::uint8_t> message(50, 0);
  message[0] = 'I';
  return message;
}

void writeU16BE(std::vector<std::uint8_t> &bytes, std::size_t offset,
                std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void writeU32BE(std::vector<std::uint8_t> &bytes, std::size_t offset,
                std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void writeU64BE(std::vector<std::uint8_t> &bytes, std::size_t offset,
                std::uint64_t value) {
  for (int index = 7; index >= 0; --index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}

void writeStock(std::vector<std::uint8_t> &message, std::size_t offset,
                std::string_view ticker) {
  for (std::size_t index = 0; index < 8; ++index) {
    message[offset + index] =
        index < ticker.size() ? static_cast<std::uint8_t>(ticker[index]) : ' ';
  }
}

std::vector<std::uint8_t> stockDirectory(std::uint16_t locate,
                                         std::string_view ticker) {
  std::vector<std::uint8_t> message(39, 0);
  message[0] = 'R';
  writeU16BE(message, 1, locate);
  writeStock(message, 11, ticker);
  message[19] = 'Q';
  message[20] = 'N';
  writeU32BE(message, 21, 100);
  message[25] = 'N';
  message[26] = 'C';
  message[29] = 'P';
  return message;
}

std::vector<std::uint8_t>
addOrder(std::uint16_t locate, std::uint64_t order_ref, char side,
         std::uint32_t quantity, std::string_view ticker,
         std::uint32_t price, bool attributed = false) {
  std::vector<std::uint8_t> message(attributed ? 40 : 36, 0);
  message[0] = attributed ? 'F' : 'A';
  writeU16BE(message, 1, locate);
  writeU64BE(message, 11, order_ref);
  message[19] = static_cast<std::uint8_t>(side);
  writeU32BE(message, 20, quantity);
  writeStock(message, 24, ticker);
  writeU32BE(message, 32, price);
  if (attributed) {
    message[36] = 'T';
    message[37] = 'E';
    message[38] = 'S';
    message[39] = 'T';
  }
  return message;
}

std::vector<std::uint8_t> cancelOrder(std::uint16_t locate,
                                      std::uint64_t order_ref,
                                      std::uint32_t quantity) {
  std::vector<std::uint8_t> message(23, 0);
  message[0] = 'X';
  writeU16BE(message, 1, locate);
  writeU64BE(message, 11, order_ref);
  writeU32BE(message, 19, quantity);
  return message;
}

std::vector<std::uint8_t>
executeOrder(std::uint16_t locate, std::uint64_t order_ref,
             std::uint32_t quantity, std::uint64_t match_number,
             std::uint32_t execution_price = 0, bool with_price = false) {
  std::vector<std::uint8_t> message(with_price ? 36 : 31, 0);
  message[0] = with_price ? 'C' : 'E';
  writeU16BE(message, 1, locate);
  writeU64BE(message, 11, order_ref);
  writeU32BE(message, 19, quantity);
  writeU64BE(message, 23, match_number);
  if (with_price) {
    message[31] = 'Y';
    writeU32BE(message, 32, execution_price);
  }
  return message;
}

std::vector<std::uint8_t>
replaceOrder(std::uint16_t locate, std::uint64_t old_order_ref,
             std::uint64_t new_order_ref, std::uint32_t quantity,
             std::uint32_t price) {
  std::vector<std::uint8_t> message(35, 0);
  message[0] = 'U';
  writeU16BE(message, 1, locate);
  writeU64BE(message, 11, old_order_ref);
  writeU64BE(message, 19, new_order_ref);
  writeU32BE(message, 27, quantity);
  writeU32BE(message, 31, price);
  return message;
}

std::vector<std::uint8_t> deleteOrder(std::uint16_t locate,
                                      std::uint64_t order_ref) {
  std::vector<std::uint8_t> message(19, 0);
  message[0] = 'D';
  writeU16BE(message, 1, locate);
  writeU64BE(message, 11, order_ref);
  return message;
}

std::vector<std::uint8_t> packetBytes(const PacketView &packet) {
  const auto *begin =
      reinterpret_cast<const std::uint8_t *>(packet.data);
  return {begin, begin + packet.size};
}

TEST(ItchMoldUdpSourceTest, EmitsExactGoldenMoldUdp64Bytes) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "TEST", 2);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  const std::vector<std::uint8_t> expected{
      'T', 'E', 'S', 'T', ' ', ' ', ' ', ' ', ' ', ' ',
      0,   0,   0,   0,   0,   0,   0,   1,
      0,   2,
      0,   12,  'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'O',
      0,   12,  'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'C',
  };
  EXPECT_EQ(packetBytes(packet), expected);
  EXPECT_EQ(source.nextSequence(), 3u);
  EXPECT_TRUE(source.completed());
  EXPECT_EQ(source.completionKind(), CompletionKind::Terminator);
  EXPECT_TRUE(source.lastError().empty());
  EXPECT_FALSE(source.next(packet));
}

TEST(ItchMoldUdpSourceTest,
     BinaryFilePipelineMaintainsIndependentCrudPriceLevelsPerSymbol) {
  constexpr std::uint16_t kFirstLocate = 7;
  constexpr std::uint16_t kSecondLocate = 8;
  constexpr std::uint32_t kSharedPrice =
      astra::protocol::kMaxItchPrice4;
  constexpr std::uint32_t kReplacementPrice = kSharedPrice - 1'000;
  constexpr std::string_view kSession = "PIPELINE01";

  std::vector<std::uint8_t> file;
  const auto record = [&](const std::vector<std::uint8_t> &message) {
    appendRecord(file, message);
  };

  record(systemEvent('O'));
  record(stockDirectory(kFirstLocate, "FIRST"));
  record(stockDirectory(kSecondLocate, "SECOND"));
  record(systemEvent('S'));

  record(systemEvent('Q'));
  record(addOrder(kFirstLocate, 101, 'B', 100, "FIRST", kSharedPrice));
  record(addOrder(kFirstLocate, 102, 'B', 40, "FIRST", kSharedPrice,
                  /*attributed=*/true));
  record(addOrder(kSecondLocate, 201, 'S', 200, "SECOND", kSharedPrice));

  record(addOrder(kSecondLocate, 202, 'S', 50, "SECOND", kSharedPrice,
                  /*attributed=*/true));
  record(cancelOrder(kFirstLocate, 101, 25));
  record(executeOrder(kSecondLocate, 201, 100, 9'001));
  record(replaceOrder(kFirstLocate, 102, 103, 60, kReplacementPrice));

  record(executeOrder(kSecondLocate, 201, 50, 9'002, kSharedPrice,
                      /*with_price=*/true));
  record(deleteOrder(kFirstLocate, 101));
  record(deleteOrder(kSecondLocate, 201));
  record(systemEvent('M'));

  record(systemEvent('E'));
  record(deleteOrder(kFirstLocate, 103));
  record(deleteOrder(kSecondLocate, 202));
  record(systemEvent('C'));
  appendTerminator(file);

  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), std::string(kSession),
                           /*msgs_per_packet=*/4);
  astra::symbol::StockDirectory symbols;
  BookManager books;
  MoldUdpDecoder decoder(symbols, books);

  PacketView packet;
  std::size_t packet_count = 0;
  while (source.next(packet)) {
    ++packet_count;
    ASSERT_EQ(decoder.processPacket(packet).status, DecodeStatus::Ok)
        << "packet " << packet_count;

    const OrderBook *const first = books.getOrderBook(kFirstLocate);
    const OrderBook *const second = books.getOrderBook(kSecondLocate);
    if (packet_count <= 2) {
      ASSERT_NE(first, nullptr);
      ASSERT_NE(second, nullptr);
      EXPECT_EQ(first->liveOrderCount(), 0u);
      EXPECT_EQ(second->liveOrderCount(), 0u);
    } else if (packet_count == 3) {
      ASSERT_NE(first, nullptr);
      ASSERT_NE(second, nullptr);
      const BookUpdate first_levels = first->getBookUpdate();
      const BookUpdate second_levels = second->getBookUpdate();
      ASSERT_EQ(first_levels.bids_depth, 1u);
      EXPECT_EQ(first_levels.bids[0].price, kSharedPrice);
      EXPECT_EQ(first_levels.bids[0].qty, 140u);
      EXPECT_EQ(first_levels.bids[0].num_orders, 2u);
      ASSERT_EQ(second_levels.asks_depth, 1u);
      EXPECT_EQ(second_levels.asks[0].price, kSharedPrice);
      EXPECT_EQ(second_levels.asks[0].qty, 250u);
      EXPECT_EQ(second_levels.asks[0].num_orders, 2u);
    } else if (packet_count == 4) {
      const BookUpdate first_levels = first->getBookUpdate();
      const BookUpdate second_levels = second->getBookUpdate();
      ASSERT_EQ(first_levels.bids_depth, 2u);
      EXPECT_EQ(first_levels.bids[0].price, kSharedPrice);
      EXPECT_EQ(first_levels.bids[0].qty, 75u);
      EXPECT_EQ(first_levels.bids[0].num_orders, 1u);
      EXPECT_EQ(first_levels.bids[1].price, kReplacementPrice);
      EXPECT_EQ(first_levels.bids[1].qty, 60u);
      EXPECT_EQ(first_levels.bids[1].num_orders, 1u);
      ASSERT_EQ(second_levels.asks_depth, 1u);
      EXPECT_EQ(second_levels.asks[0].price, kSharedPrice);
      EXPECT_EQ(second_levels.asks[0].qty, 100u);
      EXPECT_EQ(second_levels.asks[0].num_orders, 2u);
    } else if (packet_count == 5) {
      const BookUpdate first_levels = first->getBookUpdate();
      const BookUpdate second_levels = second->getBookUpdate();
      ASSERT_EQ(first_levels.bids_depth, 1u);
      EXPECT_EQ(first_levels.bids[0].price, kReplacementPrice);
      EXPECT_EQ(first_levels.bids[0].qty, 60u);
      EXPECT_EQ(first_levels.bids[0].num_orders, 1u);
      ASSERT_EQ(second_levels.asks_depth, 1u);
      EXPECT_EQ(second_levels.asks[0].price, kSharedPrice);
      EXPECT_EQ(second_levels.asks[0].qty, 50u);
      EXPECT_EQ(second_levels.asks[0].num_orders, 1u);
    }
  }

  ASSERT_EQ(packet_count, 6u);
  ASSERT_TRUE(source.completed());
  EXPECT_TRUE(source.lastError().empty());
  EXPECT_EQ(source.nextSequence(), 21u);
  ASSERT_NE(books.getOrderBook(kFirstLocate), nullptr);
  ASSERT_NE(books.getOrderBook(kSecondLocate), nullptr);
  EXPECT_EQ(books.getOrderBook(kFirstLocate)->liveOrderCount(), 0u);
  EXPECT_EQ(books.getOrderBook(kSecondLocate)->liveOrderCount(), 0u);
  EXPECT_EQ(books.getOrderBook(kFirstLocate)->getBookUpdate().bids_depth, 0u);
  EXPECT_EQ(books.getOrderBook(kSecondLocate)->getBookUpdate().asks_depth, 0u);

  const auto end_of_session =
      astra::protocol::makeMoldEndOfSessionPacket(
          kSession, source.nextSequence());
  const PacketView end_view{end_of_session.data(), end_of_session.size(), 0};
  EXPECT_EQ(decoder.processPacket(end_view).status, DecodeStatus::EndOfStream);
  EXPECT_TRUE(decoder.endOfStreamAccepted());
}

TEST(ItchMoldUdpSourceTest,
     PreservesCurrentDirectListingPayloadByteForByte) {
  std::vector<std::uint8_t> listing(48);
  listing[0] = 'O';
  for (std::size_t index = 1; index < listing.size(); ++index)
    listing[index] = static_cast<std::uint8_t>(index);

  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, listing);
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "O-WIRE", 3);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  const auto bytes = packetBytes(packet);
  ASSERT_EQ(bytes.size(), 98u);
  EXPECT_EQ(bytes[17], 1u);
  EXPECT_EQ(bytes[18], 0u);
  EXPECT_EQ(bytes[19], 3u);
  EXPECT_EQ(bytes[34], 0u);
  EXPECT_EQ(bytes[35], 48u);
  EXPECT_EQ(std::vector<std::uint8_t>(bytes.begin() + 36,
                                      bytes.begin() + 84),
            listing);
  EXPECT_TRUE(source.completed());
  EXPECT_FALSE(source.next(packet));
  EXPECT_EQ(source.nextSequence(), 4u);
}

TEST(ItchMoldUdpSourceTest,
     PacketLimitCanChangeAtLifecycleBoundaryForTimestampReplay) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('S'));
  appendRecord(file, directListing());
  appendRecord(file, noii());
  appendRecord(file, systemEvent('Q'));
  appendRecord(file, directListing());
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "TIMESTAMP", 20);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_EQ(packetBytes(packet)[19], 2u);

  EXPECT_FALSE(source.setMessagesPerPacket(0));
  ASSERT_TRUE(source.setMessagesPerPacket(1));
  for (std::uint64_t sequence = 3; sequence <= 5; ++sequence) {
    ASSERT_TRUE(source.next(packet));
    const auto bytes = packetBytes(packet);
    EXPECT_EQ(bytes[17], sequence);
    EXPECT_EQ(bytes[19], 1u);
  }

  ASSERT_TRUE(source.setMessagesPerPacket(20));
  ASSERT_TRUE(source.next(packet));
  const auto final = packetBytes(packet);
  EXPECT_EQ(final[17], 6u);
  EXPECT_EQ(final[19], 2u);
  EXPECT_TRUE(source.completed());
}

TEST(ItchMoldUdpSourceTest, SplitsPacketsAtConfiguredMessageCount) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, directListing());
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "S", 2);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  auto first = packetBytes(packet);
  ASSERT_EQ(first.size(), 84u);
  EXPECT_EQ(first[17], 1u);
  EXPECT_EQ(first[18], 0u);
  EXPECT_EQ(first[19], 2u);
  EXPECT_EQ(first[20], 0u);
  EXPECT_EQ(first[21], 12u);
  EXPECT_EQ(first[22], 'S');
  EXPECT_EQ(first[33], 'O');
  EXPECT_EQ(first[34], 0u);
  EXPECT_EQ(first[35], 48u);
  EXPECT_EQ(first[36], 'O');

  ASSERT_TRUE(source.next(packet));
  auto second = packetBytes(packet);
  ASSERT_EQ(second.size(), 34u);
  EXPECT_EQ(second[17], 3u);
  EXPECT_EQ(second[18], 0u);
  EXPECT_EQ(second[19], 1u);
  EXPECT_EQ(second[20], 0u);
  EXPECT_EQ(second[21], 12u);
  EXPECT_EQ(second[22], 'S');
  EXPECT_EQ(second[33], 'C');
  EXPECT_TRUE(source.completed());
  EXPECT_EQ(source.nextSequence(), 4u);
  EXPECT_FALSE(source.next(packet));
}

TEST(ItchMoldUdpSourceTest,
     SystemHoursAndMarketHoursEventsEndTheirMoldPackets) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, directListing());
  appendRecord(file, systemEvent('S'));
  appendRecord(file, directListing());
  appendRecord(file, noii());
  appendRecord(file, systemEvent('Q'));
  appendRecord(file, directListing());
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "BOUNDARY", 100);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  auto first = packetBytes(packet);
  ASSERT_EQ(first.size(), 98u);
  EXPECT_EQ(first[17], 1u);
  EXPECT_EQ(first[18], 0u);
  EXPECT_EQ(first[19], 3u);
  EXPECT_EQ(first[84], 0u);
  EXPECT_EQ(first[85], 12u);
  EXPECT_EQ(first[86], 'S');
  EXPECT_EQ(first[97], 'S');

  ASSERT_TRUE(source.next(packet));
  auto second = packetBytes(packet);
  ASSERT_EQ(second.size(), 136u);
  EXPECT_EQ(second[17], 4u);
  EXPECT_EQ(second[18], 0u);
  EXPECT_EQ(second[19], 3u);
  EXPECT_EQ(second[122], 0u);
  EXPECT_EQ(second[123], 12u);
  EXPECT_EQ(second[124], 'S');
  EXPECT_EQ(second[135], 'Q');

  ASSERT_TRUE(source.next(packet));
  auto third = packetBytes(packet);
  ASSERT_EQ(third.size(), 84u);
  EXPECT_EQ(third[17], 7u);
  EXPECT_EQ(third[18], 0u);
  EXPECT_EQ(third[19], 2u);
  EXPECT_EQ(third[70], 0u);
  EXPECT_EQ(third[71], 12u);
  EXPECT_EQ(third[72], 'S');
  EXPECT_EQ(third[83], 'C');

  EXPECT_TRUE(source.completed());
  EXPECT_EQ(source.nextSequence(), 9u);
  EXPECT_FALSE(source.next(packet));
}

TEST(ItchMoldUdpSourceTest, SplitsPacketsBeforeMaximumWireSize) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  for (int index = 0; index < 39; ++index)
    appendRecord(file, noii());
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "SIZE", 100);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_EQ(packet.size, 1438u);
  EXPECT_LE(packet.size,
            ItchMoldUdpSource::kStandardIpv4UdpPayloadBytes);
  EXPECT_EQ(packetBytes(packet)[18], 0u);
  EXPECT_EQ(packetBytes(packet)[19], 28u);

  ASSERT_TRUE(source.next(packet));
  EXPECT_EQ(packet.size, 658u);
  const auto bytes = packetBytes(packet);
  EXPECT_EQ(bytes[17], 29u);
  EXPECT_EQ(bytes[18], 0u);
  EXPECT_EQ(bytes[19], 13u);
  EXPECT_EQ(bytes[20], 0u);
  EXPECT_EQ(bytes[21], 50u);
  EXPECT_EQ(bytes[22], 'I');
  EXPECT_EQ(bytes[644], 0u);
  EXPECT_EQ(bytes[645], 12u);
  EXPECT_EQ(bytes[646], 'S');
  EXPECT_EQ(bytes[657], 'C');
  EXPECT_TRUE(source.completed());
}

TEST(ItchMoldUdpSourceTest, TerminatorBeforeSystemEventCIsRejected) {
  std::vector<std::uint8_t> file;
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.completionKind(), CompletionKind::None);
  EXPECT_EQ(source.nextSequence(), 1u);
  EXPECT_EQ(source.lastError(),
            "BinaryFILE terminator before ITCH System Event C");
}

TEST(ItchMoldUdpSourceTest, StrictModeRejectsRecordAlignedPhysicalEof) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('C'));
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.completionKind(), CompletionKind::None);
  EXPECT_EQ(source.lastError(),
            "physical EOF before zero-length BinaryFILE terminator");
  EXPECT_FALSE(source.next(packet));
}

TEST(ItchMoldUdpSourceTest, LegacyModeOnlyAcceptsEofImmediatelyAfterSystemEventC) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('C'));
  const BinaryFile input(file);
  ItchMoldUdpSource source(
      input.path().string(), "LEGACY", 20,
      CompletionPolicy::AllowEofAfterEndOfMessages);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_TRUE(source.completed());
  EXPECT_EQ(source.completionKind(), CompletionKind::LegacyEndOfMessagesEof);
  EXPECT_TRUE(source.lastError().empty());
  EXPECT_FALSE(source.next(packet));
}

TEST(ItchMoldUdpSourceTest, LegacyModeRejectsArbitraryRecordAlignedEof) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  const BinaryFile input(file);
  ItchMoldUdpSource source(
      input.path().string(), "LEGACY", 20,
      CompletionPolicy::AllowEofAfterEndOfMessages);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(),
            "physical EOF before zero-length BinaryFILE terminator");
}

TEST(ItchMoldUdpSourceTest, RejectsBytesAfterTerminator) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  file.push_back(0x7F);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(),
            "data follows zero-length BinaryFILE terminator");
  EXPECT_FALSE(source.next(packet));
}

TEST(ItchMoldUdpSourceTest, RejectsTruncatedLengthPrefix) {
  const BinaryFile input({0x00});
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(), "truncated ITCH message-length prefix");
}

TEST(ItchMoldUdpSourceTest, RejectsTruncatedMessagePayload) {
  const BinaryFile input({0x00, 0x0C, 'S', 0x00});
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(), "truncated ITCH message payload");
  EXPECT_EQ(source.nextSequence(), 1u);
}

TEST(ItchMoldUdpSourceTest, RejectsSingleMessageLargerThanPacketCapacity) {
  std::vector<std::uint8_t> file;
  appendRecord(file, std::vector<std::uint8_t>(2027, 0xA5));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(),
            "ITCH message is too large for a MoldUDP packet");
}

TEST(ItchMoldUdpSourceTest, RejectsUnsupportedTypeWithoutSendingIt) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, std::vector<std::uint8_t>(12, 'Z'));
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  const auto bytes = packetBytes(packet);
  EXPECT_EQ(bytes.size(), 34u);
  EXPECT_EQ(bytes[19], 1u);
  EXPECT_EQ(bytes[22], 'S');
  EXPECT_EQ(source.lastError(), "unsupported ITCH message type");
  EXPECT_FALSE(source.completed());
}

TEST(ItchMoldUdpSourceTest, RejectsWrongLengthWithoutTrailingPacketBytes) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  std::vector<std::uint8_t> wrong_add(35, 0);
  wrong_add[0] = 'A';
  appendRecord(file, wrong_add);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  const auto bytes = packetBytes(packet);
  EXPECT_EQ(bytes.size(), 34u);
  EXPECT_EQ(bytes[19], 1u);
  EXPECT_EQ(source.lastError(), "invalid ITCH message length");
  EXPECT_FALSE(source.completed());
}

TEST(ItchMoldUdpSourceTest, RejectsRecordAfterSystemEventC) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('C'));
  appendRecord(file, directListing());
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_EQ(packetBytes(packet)[19], 2u);
  EXPECT_EQ(source.lastError(), "ITCH record follows System Event C");
  EXPECT_FALSE(source.completed());
}

TEST(ItchMoldUdpSourceTest, RequiresSystemEventOAsFirstRecord) {
  std::vector<std::uint8_t> file;
  appendRecord(file, directListing());
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_EQ(source.lastError(),
            "first ITCH record is not System Event O");
}

TEST(ItchMoldUdpSourceTest, RejectsNonzeroSystemEventStockLocate) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O', /*locate=*/7));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string());

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_EQ(source.nextSequence(), 1u);
  EXPECT_EQ(source.lastError(),
            "System Event stock locate is not zero");
}

TEST(ItchMoldUdpSourceTest, RejectsSessionLongerThanMoldField) {
  std::vector<std::uint8_t> file;
  appendRecord(file, systemEvent('O'));
  appendRecord(file, systemEvent('C'));
  appendTerminator(file);
  const BinaryFile input(file);
  ItchMoldUdpSource source(input.path().string(), "ELEVEN-BYTES");

  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(source.lastError(),
            "MoldUDP64 session must not exceed 10 bytes");
}

} // namespace
