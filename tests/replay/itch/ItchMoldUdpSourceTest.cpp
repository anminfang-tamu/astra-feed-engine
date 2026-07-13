#include "replay/itch/ItchMoldUdpSource.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TemporaryItchFile {
public:
  explicit TemporaryItchFile(const std::vector<unsigned char> &bytes) {
    const auto unique = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    path_ = std::filesystem::temp_directory_path() /
            ("astra-itch-source-" + std::to_string(unique) + ".bin");
    std::ofstream output(path_, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  ~TemporaryItchFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

TEST(ItchMoldUdpSourceTest, DistinguishesCleanEofFromTruncatedInput) {
  TemporaryItchFile file({0x00, 0x01, static_cast<unsigned char>('S')});
  ItchMoldUdpSource source(file.path().string());
  source.setTimestampingEnabled(false);

  PacketView packet;
  ASSERT_TRUE(source.next(packet));
  EXPECT_EQ(packet.size, ItchMoldUdpSource::kMoldHeaderSize + 3u);
  EXPECT_TRUE(source.completed());

  EXPECT_FALSE(source.next(packet));
  EXPECT_TRUE(source.completed());
  EXPECT_TRUE(source.lastError().empty());
}

TEST(ItchMoldUdpSourceTest, TruncatedPayloadIsAReplayError) {
  TemporaryItchFile file({0x00, 0x02, static_cast<unsigned char>('S')});
  ItchMoldUdpSource source(file.path().string());
  source.setTimestampingEnabled(false);

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(), "truncated ITCH message payload");
}

TEST(ItchMoldUdpSourceTest, RejectsZeroMessagesPerPacket) {
  TemporaryItchFile file({0x00, 0x01, static_cast<unsigned char>('S')});
  ItchMoldUdpSource source(file.path().string(), "ASTRA     ", 0);
  source.setTimestampingEnabled(false);

  PacketView packet;
  EXPECT_FALSE(source.next(packet));
  EXPECT_FALSE(source.completed());
  EXPECT_EQ(source.lastError(), "msgs_per_packet must be greater than zero");
}

} // namespace
