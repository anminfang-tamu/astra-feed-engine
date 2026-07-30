#include "astra/source/UdpReceiver.hpp"
#include "astra/source/PacketView.hpp"
#include "astra/source/UdpBatchReceiver.hpp"
#include "astra/source/UdpSender.hpp"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

class ScopedEnvironmentVariable {
public:
  ScopedEnvironmentVariable(const char *name, const char *value)
      : name_(name) {
    if (const char *existing = std::getenv(name); existing != nullptr) {
      had_original_ = true;
      original_ = existing;
    }
    if (value == nullptr) {
      ::unsetenv(name);
    } else {
      ::setenv(name, value, 1);
    }
  }

  ~ScopedEnvironmentVariable() {
    if (had_original_) {
      ::setenv(name_.c_str(), original_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
  ScopedEnvironmentVariable &
  operator=(const ScopedEnvironmentVariable &) = delete;

private:
  std::string name_;
  std::string original_;
  bool had_original_{false};
};

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

  void send_bytes(const UdpReceiver &receiver, const void *data,
                  std::size_t len) {
    struct sockaddr_in local{};
    socklen_t local_length = sizeof(local);
    ASSERT_EQ(::getsockname(
                  receiver.fd(), reinterpret_cast<struct sockaddr *>(&local),
                  &local_length),
              0);
    ASSERT_EQ(local.sin_family, AF_INET);
    ASSERT_NE(local.sin_port, 0);

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = local.sin_port;
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
  UdpReceiver rx("127.0.0.1", 0);
  PacketView pkt;
  EXPECT_FALSE(rx.next(pkt));
}

TEST_F(UdpReceiverTest, ReceivesUnicastPacket) {
  UdpReceiver rx("127.0.0.1", 0);

  const char payload[] = "hello_hft";
  send_bytes(rx, payload, sizeof(payload));

  PacketView pkt = spin_next(rx);

  EXPECT_EQ(pkt.size, sizeof(payload));
  EXPECT_EQ(std::memcmp(pkt.data, payload, sizeof(payload)), 0);
}

TEST_F(UdpReceiverTest, ReceiveStartTicksAreNonZero) {
  UdpReceiver rx("127.0.0.1", 0);

  const char payload[] = "ts";
  send_bytes(rx, payload, sizeof(payload));

  PacketView pkt = spin_next(rx);

  EXPECT_GT(pkt.receive_start_ticks, 0u);
}

TEST_F(UdpReceiverTest, MultiplePacketsReceivedInOrder) {
  UdpReceiver rx("127.0.0.1", 0);

  for (uint8_t i = 0; i < 5; ++i)
    send_bytes(rx, &i, sizeof(i));

  for (uint8_t i = 0; i < 5; ++i) {
    PacketView pkt = spin_next(rx);
    ASSERT_EQ(pkt.size, 1u);
    EXPECT_EQ(static_cast<uint8_t>(*pkt.data), i) << "packet " << (int)i << " out of order";
  }
}

TEST_F(UdpReceiverTest, ThrowsOnInvalidIpAddress) {
  EXPECT_THROW(UdpReceiver("not_an_ip", 0), std::runtime_error);
}

TEST_F(UdpReceiverTest, RejectsUnknownDropMetricsSetting) {
  const ScopedEnvironmentVariable setting("ASTRA_UDP_DROP_METRICS",
                                           "definitely-not-a-mode");
  EXPECT_THROW(UdpReceiver("127.0.0.1", 0), std::runtime_error);
}

TEST_F(UdpReceiverTest, RequestedDropMetricsAreEnabledOrFailAtStartup) {
  const ScopedEnvironmentVariable setting("ASTRA_UDP_DROP_METRICS", "on");
#if defined(__linux__) && defined(SO_RXQ_OVFL)
  UdpReceiver receiver("127.0.0.1", 0);
  EXPECT_TRUE(receiver.dropMetricsEnabled());
#else
  EXPECT_THROW(UdpReceiver("127.0.0.1", 0), std::runtime_error);
#endif
}

TEST_F(UdpReceiverTest, ConstructorFailureDoesNotLeakSocket) {
  const int blocker_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(blocker_fd, 0);

  struct sockaddr_in blocker_address{};
  blocker_address.sin_family = AF_INET;
  blocker_address.sin_port = 0;
  blocker_address.sin_addr.s_addr = htonl(INADDR_ANY);
  ASSERT_EQ(::bind(blocker_fd,
                   reinterpret_cast<struct sockaddr *>(&blocker_address),
                   sizeof(blocker_address)),
            0);

  socklen_t address_length = sizeof(blocker_address);
  ASSERT_EQ(::getsockname(
                blocker_fd,
                reinterpret_cast<struct sockaddr *>(&blocker_address),
                &address_length),
            0);
  const uint16_t blocked_port = ntohs(blocker_address.sin_port);
  ASSERT_NE(blocked_port, 0);

  const int first_available =
      ::fcntl(sender_fd_, F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(first_available, 0);
  ASSERT_EQ(::close(first_available), 0);

  EXPECT_THROW(UdpReceiver("127.0.0.1", blocked_port),
               std::runtime_error);

  const int available_after_failure =
      ::fcntl(sender_fd_, F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(available_after_failure, 0);
  EXPECT_EQ(available_after_failure, first_available);

  EXPECT_EQ(::close(available_after_failure), 0);
  EXPECT_EQ(::close(blocker_fd), 0);
}

TEST(UdpSenderTest, InvalidAddressDoesNotOpenSocket) {
  const int anchor_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(anchor_fd, 0);

  const int first_available =
      ::fcntl(anchor_fd, F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(first_available, 0);
  ASSERT_EQ(::close(first_available), 0);

  EXPECT_THROW(UdpSender("not_an_ip", 12345), std::runtime_error);

  const int available_after_failure =
      ::fcntl(anchor_fd, F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(available_after_failure, 0);
  EXPECT_EQ(available_after_failure, first_available);

  EXPECT_EQ(::close(available_after_failure), 0);
  EXPECT_EQ(::close(anchor_fd), 0);
}

TEST(UdpSenderTest, DescriptorOwnershipIsUnique) {
  static_assert(!std::is_copy_constructible_v<UdpSender>);
  static_assert(!std::is_copy_assignable_v<UdpSender>);
  static_assert(!std::is_move_constructible_v<UdpSender>);
  static_assert(!std::is_move_assignable_v<UdpSender>);
  SUCCEED();
}

#ifdef __linux__
TEST(UdpBatchReceiverTest, ConstructorFailureDoesNotLeakSocket) {
  const int blocker_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(blocker_fd, 0);

  struct sockaddr_in blocker_address{};
  blocker_address.sin_family = AF_INET;
  blocker_address.sin_port = 0;
  blocker_address.sin_addr.s_addr = htonl(INADDR_ANY);
  ASSERT_EQ(::bind(blocker_fd,
                   reinterpret_cast<struct sockaddr *>(&blocker_address),
                   sizeof(blocker_address)),
            0);

  socklen_t address_length = sizeof(blocker_address);
  ASSERT_EQ(::getsockname(
                blocker_fd,
                reinterpret_cast<struct sockaddr *>(&blocker_address),
                &address_length),
            0);
  const uint16_t blocked_port = ntohs(blocker_address.sin_port);
  ASSERT_NE(blocked_port, 0);

  const int first_available =
      ::fcntl(blocker_fd, F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(first_available, 0);
  ASSERT_EQ(::close(first_available), 0);

  EXPECT_THROW(UdpBatchReceiver("127.0.0.1", blocked_port),
               std::runtime_error);

  const int available_after_failure =
      ::fcntl(blocker_fd, F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(available_after_failure, 0);
  EXPECT_EQ(available_after_failure, first_available);

  EXPECT_EQ(::close(available_after_failure), 0);
  EXPECT_EQ(::close(blocker_fd), 0);
}
#endif

TEST(UdpBatchReceiverPolicyTest, DropsRatherThanClampsTruncatedDatagrams) {
  EXPECT_FALSE(UdpBatchReceiver::shouldDropDatagram(
      UdpBatchReceiver::kMaxPacketSize, false));
  EXPECT_TRUE(UdpBatchReceiver::shouldDropDatagram(
      UdpBatchReceiver::kMaxPacketSize + 1, false));
  EXPECT_TRUE(UdpBatchReceiver::shouldDropDatagram(128, true));
}
