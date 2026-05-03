#include <cstddef>
#include <cstdint>

struct PacketView {
  const std::byte *data{nullptr};
  std::size_t size{0};
  uint64_t receive_ts_ns{0};
};