#include "astra/source/UdpReceiver.hpp"
#include "astra/source/PacketView.hpp"
#include "astra/source/UdpBatchReceiver.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

// Port unlikely to be in use; SO_REUSEADDR in UdpReceiver handles reruns
static constexpr uint16_t kPort = 54321;

} // namespace

class UdpReceiverTest : public ::testing::Test {
protected:
  void SetUp() override {
    sender_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sender_fd_, 0);
  }

  void TearDown() override {
    if (sender_fd_ >= 0)
      ::close(sender_fd_);
  }

  void send_bytes(const void *data, std::size_t len) {
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kPort);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto sent = ::sendto(sender_fd_, data, len, 0,
                         reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
    ASSERT_EQ(sent, static_cast<ssize_t>(len));
  }

  // Spin next() up to max_spins times; fails the test if nothing arrives
  PacketView spin_next(UdpReceiver &rx, int max_spins = 10'000) {
    PacketView pkt;
    for (int i = 0; i < max_spins; ++i)
      if (rx.next(pkt))
        return pkt;
    ADD_FAILURE() << "packet not received within " << max_spins << " spins";
    return pkt;
  }

  int sender_fd_ = -1;
};

TEST_F(UdpReceiverTest, NextReturnsFalseWhenNoData) {
  UdpReceiver rx("127.0.0.1", kPort);
  PacketView pkt;
  EXPECT_FALSE(rx.next(pkt));
}

TEST_F(UdpReceiverTest, ReceivesUnicastPacket) {
  UdpReceiver rx("127.0.0.1", kPort);

  const char payload[] = "hello_hft";
  send_bytes(payload, sizeof(payload));

  PacketView pkt = spin_next(rx);

  EXPECT_EQ(pkt.size, sizeof(payload));
  EXPECT_EQ(std::memcmp(pkt.data, payload, sizeof(payload)), 0);
}

TEST_F(UdpReceiverTest, ReceiveStartTicksAreNonZero) {
  UdpReceiver rx("127.0.0.1", kPort);

  const char payload[] = "ts";
  send_bytes(payload, sizeof(payload));

  PacketView pkt = spin_next(rx);

  EXPECT_GT(pkt.receive_start_ticks, 0u);
}

TEST_F(UdpReceiverTest, MultiplePacketsReceivedInOrder) {
  UdpReceiver rx("127.0.0.1", kPort);

  for (uint8_t i = 0; i < 5; ++i)
    send_bytes(&i, sizeof(i));

  for (uint8_t i = 0; i < 5; ++i) {
    PacketView pkt = spin_next(rx);
    ASSERT_EQ(pkt.size, 1u);
    EXPECT_EQ(static_cast<uint8_t>(*pkt.data), i) << "packet " << (int)i << " out of order";
  }
}

TEST_F(UdpReceiverTest, ThrowsOnInvalidIpAddress) {
  EXPECT_THROW(UdpReceiver("not_an_ip", kPort), std::runtime_error);
}

TEST(UdpBatchReceiverPolicyTest, DropsRatherThanClampsTruncatedDatagrams) {
  EXPECT_FALSE(UdpBatchReceiver::shouldDropDatagram(
      UdpBatchReceiver::kMaxPacketSize, false));
  EXPECT_TRUE(UdpBatchReceiver::shouldDropDatagram(
      UdpBatchReceiver::kMaxPacketSize + 1, false));
  EXPECT_TRUE(UdpBatchReceiver::shouldDropDatagram(128, true));
}
