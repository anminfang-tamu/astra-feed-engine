#pragma once

#include "IMarketDataSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/uio.h>
#endif

class UdpBatchReceiver : public IMarketDataSource {
public:
  static constexpr std::size_t kMaxPacketSize = 2048;
  static constexpr std::size_t kMaxBatchSize = 64;
  static constexpr std::size_t kDefaultBatchSize = 8;

  UdpBatchReceiver(const char *ip, uint16_t port,
                   std::size_t batch_size = kDefaultBatchSize);
  ~UdpBatchReceiver();

  UdpBatchReceiver(const UdpBatchReceiver &) = delete;
  UdpBatchReceiver &operator=(const UdpBatchReceiver &) = delete;
  UdpBatchReceiver(UdpBatchReceiver &&) = delete;
  UdpBatchReceiver &operator=(UdpBatchReceiver &&) = delete;

  // Compatibility path for the current engine: refill with recvmmsg(), then
  // return one packet per call.
  bool next(PacketView &packet) noexcept override;

  // Batch path for experiments or a future engine loop that can process
  // several UDP datagrams before returning to the socket.
  std::size_t nextBatch(PacketView *packets, std::size_t max_packets) noexcept;

  uint64_t errors() const noexcept { return error_count_; }
  uint64_t truncated() const noexcept { return truncated_count_; }
  uint64_t syscalls() const noexcept { return syscall_count_; }
  uint64_t kernelDrops() const noexcept { return kernel_drop_count_; }

  int fd() const noexcept { return fd_; }

private:
  std::size_t receiveBatch() noexcept;
#ifdef __linux__
  void recordKernelDrops(const struct msghdr &msg) noexcept;
#endif

  int fd_{-1};
  std::size_t batch_size_{kDefaultBatchSize};

  std::array<PacketView, kMaxBatchSize> pending_{};
  std::size_t pending_index_{0};
  std::size_t pending_count_{0};

#ifdef __linux__
  alignas(64) std::array<std::array<std::byte, kMaxPacketSize>,
                         kMaxBatchSize> buffers_{};
  std::array<struct mmsghdr, kMaxBatchSize> msgs_{};
  std::array<struct iovec, kMaxBatchSize> iovecs_{};
  alignas(struct cmsghdr)
      std::array<std::array<std::byte, 64>, kMaxBatchSize> controls_{};
#endif

  uint64_t error_count_{0};
  uint64_t truncated_count_{0};
  uint64_t syscall_count_{0};
  uint64_t kernel_drop_count_{0};
#ifdef __linux__
  uint32_t last_kernel_drop_count_{0};
#endif
};
