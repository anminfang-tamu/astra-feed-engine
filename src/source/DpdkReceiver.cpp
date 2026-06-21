#include "astra/source/DpdkReceiver.hpp"

#include "astra/core/Time.hpp"
#include "astra/source/PacketView.hpp"

#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_udp.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint16_t kQinqEtherType = 0x88A8;
constexpr uint16_t kIpv4FragmentOffsetMask = 0x1FFF;
constexpr uint16_t kIpv4MoreFragmentsFlag = 0x2000;
constexpr unsigned int kDefaultMempoolSize = 65535;
constexpr unsigned int kDefaultMbufCacheSize = 256;
constexpr uint16_t kDefaultRxDesc = 4096;
constexpr std::size_t kMaxBurstSize = 256;
constexpr uint8_t kIpv4NoOptionsVersionIhl = 0x45;

enum class LatencyMode { Packet, Burst };

bool envFlag(const char *name, bool fallback) noexcept {
  const char *value = std::getenv(name);
  if (value == nullptr)
    return fallback;
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
      std::strcmp(value, "off") == 0 || std::strcmp(value, "no") == 0)
    return false;
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
      std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0)
    return true;
  return fallback;
}

unsigned long envUnsigned(const char *name, unsigned long fallback,
                          unsigned long minimum,
                          unsigned long maximum) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0')
    return fallback;

  char *end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    throw std::runtime_error(std::string("invalid unsigned integer in ") +
                             name + ": " + value);
  }

  return std::clamp(parsed, minimum, maximum);
}

std::size_t dpdkBurstSize() {
  const auto burst_size = static_cast<std::size_t>(
      envUnsigned("ASTRA_DPDK_BURST_SIZE", DpdkReceiver::kDefaultBurstSize, 1,
                  kMaxBurstSize));
  if (burst_size % 8 != 0) {
    throw std::runtime_error(
        "ASTRA_DPDK_BURST_SIZE must be divisible by 8");
  }
  return burst_size;
}

LatencyMode dpdkLatencyMode() {
  const char *value = std::getenv("ASTRA_DPDK_LATENCY_MODE");
  if (value == nullptr || value[0] == '\0' ||
      std::strcmp(value, "packet") == 0) {
    return LatencyMode::Packet;
  }
  if (std::strcmp(value, "burst") == 0)
    return LatencyMode::Burst;

  throw std::runtime_error(
      std::string("invalid ASTRA_DPDK_LATENCY_MODE: ") + value +
      " (expected packet or burst)");
}

std::string dpdkError(const char *what, int ret) {
  const int errnum = ret < 0 ? -ret : rte_errno;
  std::string message = what;
  if (errnum != 0) {
    message += ": ";
    message += rte_strerror(errnum);
  }
  return message;
}

[[noreturn]] void throwDpdk(const char *what, int ret) {
  throw std::runtime_error(dpdkError(what, ret));
}

std::vector<std::string> splitArgs(std::string_view args) {
  std::vector<std::string> result;
  std::string current;
  bool in_single_quote = false;
  bool in_double_quote = false;

  for (char ch : args) {
    if (ch == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
      continue;
    }
    if (ch == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
      continue;
    }
    if ((ch == ' ' || ch == '\t' || ch == '\n') && !in_single_quote &&
        !in_double_quote) {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }

  if (in_single_quote || in_double_quote)
    throw std::runtime_error("unclosed quote in ASTRA_DPDK_EAL_ARGS");
  if (!current.empty())
    result.push_back(current);
  return result;
}

void initializeEalOnce() {
  static std::mutex mutex;
  static bool initialized = false;

  std::lock_guard<std::mutex> lock(mutex);
  if (initialized)
    return;

  std::vector<std::string> args{"astra-dpdk"};
  if (const char *env_args = std::getenv("ASTRA_DPDK_EAL_ARGS")) {
    std::vector<std::string> extra = splitArgs(env_args);
    args.insert(args.end(), extra.begin(), extra.end());
  }

  std::vector<char *> argv;
  argv.reserve(args.size());
  for (std::string &arg : args)
    argv.push_back(arg.data());

  const int ret = rte_eal_init(static_cast<int>(argv.size()), argv.data());
  if (ret < 0)
    throwDpdk("rte_eal_init", ret);

  initialized = true;
}

bool isVlanEtherType(uint16_t ether_type) noexcept {
  return ether_type == RTE_ETHER_TYPE_VLAN || ether_type == kQinqEtherType;
}

uint64_t currentThreadId() noexcept {
  return static_cast<uint64_t>(::syscall(SYS_gettid));
}

struct EndpointFilter {
  rte_be32_t ip{0};
  uint16_t port{0};
  bool match_ip{false};
  bool enabled{false};

  bool matches(rte_be32_t dst_ip, uint16_t dst_port) const noexcept {
    return enabled && dst_port == port && (!match_ip || dst_ip == ip);
  }
};

EndpointFilter makeEndpoint(const char *ip, uint16_t port) {
  EndpointFilter endpoint{};
  endpoint.port = port;
  endpoint.enabled = true;

  if (ip == nullptr || std::strcmp(ip, "") == 0 ||
      std::strcmp(ip, "*") == 0 || std::strcmp(ip, "any") == 0 ||
      std::strcmp(ip, "0.0.0.0") == 0) {
    return endpoint;
  }

  struct in_addr addr {};
  if (::inet_pton(AF_INET, ip, &addr) != 1) {
    throw std::runtime_error(std::string("inet_pton: bad address: ") + ip);
  }

  endpoint.ip = addr.s_addr;
  endpoint.match_ip = true;
  return endpoint;
}

enum class ParseStatus { Accepted, Filtered, Malformed, Unsupported };

struct ParsedPacket {
  ParseStatus status{ParseStatus::Filtered};
  const std::byte *payload{nullptr};
  std::size_t payload_size{0};
  uint8_t line_index{0};
  bool fast_path{false};
};

} // namespace

struct DpdkReceiver::Impl {
  Impl(const char *ip, uint16_t port)
      : line_a_(makeEndpoint(ip, port)), burst_size_(dpdkBurstSize()),
        burst_(burst_size_), latency_mode_(dpdkLatencyMode()) {
    initialize();
  }

  Impl(const char *ip_a, uint16_t port_a, const char *ip_b, uint16_t port_b)
      : line_a_(makeEndpoint(ip_a, port_a)),
        line_b_(makeEndpoint(ip_b, port_b)), burst_size_(dpdkBurstSize()),
        burst_(burst_size_), latency_mode_(dpdkLatencyMode()) {
    initialize();
  }

  ~Impl() {
    releaseDelivered();
    releaseCachedBurst();

    if (started_)
      (void)rte_eth_dev_stop(port_id_);
    if (configured_)
      (void)rte_eth_dev_close(port_id_);
    if (mempool_ != nullptr)
      rte_mempool_free(mempool_);
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool next(PacketView &packet) {
    ensureSingleThreadCaller();
    releaseDelivered();

    for (;;) {
      if (burst_index_ >= burst_count_) {
        if (!receiveBurst())
          return false;
      }

      struct rte_mbuf *mbuf = burst_[burst_index_++];
      const uint64_t packet_receive_start_ticks =
          latency_mode_ == LatencyMode::Burst ? burst_receive_start_ticks_
                                              : rdtsc();
      ParsedPacket parsed = parse(mbuf);
      if (parsed.fast_path) {
        ++fast_path_count_;
      } else {
        ++fallback_path_count_;
      }

      if (parsed.status == ParseStatus::Filtered) {
        ++filtered_count_;
        rte_pktmbuf_free(mbuf);
        continue;
      }
      if (parsed.status == ParseStatus::Malformed) {
        ++malformed_count_;
        rte_pktmbuf_free(mbuf);
        continue;
      }

      if (parsed.line_index == 0) {
        ++packets_a_;
      } else {
        ++packets_b_;
      }

      packet.data = parsed.payload;
      packet.size = parsed.payload_size;
      packet.receive_start_ticks = packet_receive_start_ticks;
      packet.line_index = parsed.line_index;
      delivered_mbuf_ = mbuf;
      return true;
    }
  }

  uint64_t imissed() const noexcept { return ethStats().imissed; }
  uint64_t ierrors() const noexcept { return ethStats().ierrors; }
  uint64_t rxNombuf() const noexcept { return ethStats().rx_nombuf; }

  EndpointFilter line_a_{};
  EndpointFilter line_b_{};
  uint16_t port_id_{0};
  uint16_t queue_id_{0};
  uint16_t rx_desc_{kDefaultRxDesc};
  int socket_id_{0};
  unsigned int mempool_size_{kDefaultMempoolSize};
  unsigned int mbuf_cache_size_{kDefaultMbufCacheSize};
  bool promiscuous_{false};
  bool all_multicast_{true};
  bool configured_{false};
  bool started_{false};

  struct rte_eth_dev_info dev_info_ {};
  struct rte_mempool *mempool_{nullptr};
  std::size_t burst_size_{DpdkReceiver::kDefaultBurstSize};
  std::vector<struct rte_mbuf *> burst_;
  LatencyMode latency_mode_{LatencyMode::Packet};
  std::size_t burst_index_{0};
  std::size_t burst_count_{0};
  uint64_t burst_receive_start_ticks_{0};
  struct rte_mbuf *delivered_mbuf_{nullptr};

  uint64_t packets_a_{0};
  uint64_t packets_b_{0};
  uint64_t filtered_count_{0};
  uint64_t malformed_count_{0};
  uint64_t fast_path_count_{0};
  uint64_t fallback_path_count_{0};
  uint64_t poll_count_{0};
  uint64_t empty_poll_count_{0};
  std::atomic<uint64_t> owner_thread_id_{0};

private:
  void initialize() {
    initializeEalOnce();

    port_id_ = static_cast<uint16_t>(
        envUnsigned("ASTRA_DPDK_PORT_ID", 0, 0,
                    std::numeric_limits<uint16_t>::max()));
    queue_id_ = static_cast<uint16_t>(
        envUnsigned("ASTRA_DPDK_QUEUE_ID", 0, 0,
                    std::numeric_limits<uint16_t>::max()));
    if (queue_id_ != 0) {
      // Multi-queue support requires explicit RSS or flow steering so the two
      // redundant lines do not race one logical MoldUDP sequence stream.
      throw std::runtime_error(
          "DpdkReceiver is single-queue in this stage; set "
          "ASTRA_DPDK_QUEUE_ID=0");
    }

    if (rte_eth_dev_count_avail() == 0)
      throw std::runtime_error("no DPDK Ethernet ports are available");
    if (!rte_eth_dev_is_valid_port(port_id_))
      throw std::runtime_error("ASTRA_DPDK_PORT_ID is not a valid DPDK port");

    int ret = rte_eth_dev_info_get(port_id_, &dev_info_);
    if (ret != 0)
      throwDpdk("rte_eth_dev_info_get", ret);
    if (dev_info_.max_rx_queues < 1) {
      throw std::runtime_error(
          "DPDK port does not report any supported RX queues");
    }

    rx_desc_ = static_cast<uint16_t>(
        envUnsigned("ASTRA_DPDK_RX_DESC", kDefaultRxDesc, 1,
                    std::numeric_limits<uint16_t>::max()));
    mempool_size_ = static_cast<unsigned int>(
        envUnsigned("ASTRA_DPDK_MEMPOOL_SIZE", kDefaultMempoolSize, 1024,
                    std::numeric_limits<unsigned int>::max()));
    mbuf_cache_size_ = static_cast<unsigned int>(
        envUnsigned("ASTRA_DPDK_MBUF_CACHE_SIZE", kDefaultMbufCacheSize, 0,
                    std::numeric_limits<unsigned int>::max()));
    promiscuous_ = envFlag("ASTRA_DPDK_PROMISCUOUS", false);
    all_multicast_ = envFlag("ASTRA_DPDK_ALLMULTICAST", true);

    socket_id_ = rte_eth_dev_socket_id(port_id_);
    if (std::getenv("ASTRA_DPDK_SOCKET_ID") != nullptr) {
      socket_id_ = static_cast<int>(envUnsigned("ASTRA_DPDK_SOCKET_ID", 0, 0,
                                                std::numeric_limits<int>::max()));
    }
    if (socket_id_ < 0)
      socket_id_ = static_cast<int>(rte_socket_id());

    std::ostringstream pool_name;
    pool_name << "astra_rx_pool_" << port_id_;
    mempool_ = rte_pktmbuf_pool_create(
        pool_name.str().c_str(), mempool_size_, mbuf_cache_size_, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, socket_id_);
    if (mempool_ == nullptr)
      throwDpdk("rte_pktmbuf_pool_create", -rte_errno);

    struct rte_eth_conf port_conf {};
    ret = rte_eth_dev_configure(port_id_, 1, 0, &port_conf);
    if (ret < 0)
      throwDpdk("rte_eth_dev_configure", ret);
    configured_ = true;

    uint16_t tx_desc = rx_desc_;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id_, &rx_desc_, &tx_desc);
    if (ret < 0)
      throwDpdk("rte_eth_dev_adjust_nb_rx_tx_desc", ret);

    struct rte_eth_rxconf rx_conf = dev_info_.default_rxconf;
    rx_conf.offloads = port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(port_id_, queue_id_, rx_desc_, socket_id_,
                                 &rx_conf, mempool_);
    if (ret < 0)
      throwDpdk("rte_eth_rx_queue_setup", ret);

    ret = rte_eth_dev_start(port_id_);
    if (ret < 0)
      throwDpdk("rte_eth_dev_start", ret);
    started_ = true;

    if (promiscuous_)
      (void)rte_eth_promiscuous_enable(port_id_);
    if (all_multicast_)
      (void)rte_eth_allmulticast_enable(port_id_);
  }

  void ensureSingleThreadCaller() {
    const uint64_t current_thread = currentThreadId();
    uint64_t expected = 0;
    if (owner_thread_id_.compare_exchange_strong(expected, current_thread))
      return;
    if (expected != current_thread) {
      throw std::runtime_error(
          "DpdkReceiver::next() must be called by only one thread per RX queue");
    }
  }

  bool receiveBurst() noexcept {
    burst_index_ = 0;
    burst_count_ = 0;
    burst_receive_start_ticks_ = rdtsc();

    const uint16_t received =
        rte_eth_rx_burst(port_id_, queue_id_, burst_.data(),
                         static_cast<uint16_t>(burst_size_));
    ++poll_count_;
    if (received == 0) {
      ++empty_poll_count_;
      return false;
    }

    burst_count_ = received;
    return true;
  }

  ParsedPacket parse(struct rte_mbuf *mbuf) const noexcept {
    if (!rte_pktmbuf_is_contiguous(mbuf))
      return {ParseStatus::Malformed};

    const std::size_t frame_len = rte_pktmbuf_pkt_len(mbuf);
    const auto *frame = reinterpret_cast<const std::byte *>(
        rte_pktmbuf_mtod(mbuf, const void *));

    ParsedPacket packet = parseFast(frame, frame_len);
    if (packet.status != ParseStatus::Unsupported)
      return packet;

    return parseGeneral(frame, frame_len);
  }

  ParsedPacket parseFast(const std::byte *frame,
                         std::size_t frame_len) const noexcept {
    constexpr std::size_t ether_offset = 0;
    constexpr std::size_t ipv4_offset = sizeof(struct rte_ether_hdr);
    constexpr std::size_t udp_offset =
        ipv4_offset + sizeof(struct rte_ipv4_hdr);
    constexpr std::size_t payload_offset =
        udp_offset + sizeof(struct rte_udp_hdr);

    if (frame_len < payload_offset)
      return {ParseStatus::Malformed, nullptr, 0, 0, true};

    const auto *ether =
        reinterpret_cast<const struct rte_ether_hdr *>(frame + ether_offset);
    const uint16_t ether_type = rte_be_to_cpu_16(ether->ether_type);
    if (ether_type != RTE_ETHER_TYPE_IPV4) {
      if (isVlanEtherType(ether_type))
        return {ParseStatus::Unsupported};
      return {ParseStatus::Filtered, nullptr, 0, 0, true};
    }

    const auto *ipv4 =
        reinterpret_cast<const struct rte_ipv4_hdr *>(frame + ipv4_offset);
    if (ipv4->version_ihl != kIpv4NoOptionsVersionIhl)
      return {ParseStatus::Unsupported};

    const uint16_t total_len = rte_be_to_cpu_16(ipv4->total_length);
    if (total_len < sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) ||
        ipv4_offset + static_cast<std::size_t>(total_len) > frame_len) {
      return {ParseStatus::Malformed, nullptr, 0, 0, true};
    }

    const uint16_t fragment_offset = rte_be_to_cpu_16(ipv4->fragment_offset);
    if ((fragment_offset &
         (kIpv4FragmentOffsetMask | kIpv4MoreFragmentsFlag)) != 0) {
      return {ParseStatus::Malformed, nullptr, 0, 0, true};
    }

    if (ipv4->next_proto_id != IPPROTO_UDP)
      return {ParseStatus::Filtered, nullptr, 0, 0, true};

    const auto *udp =
        reinterpret_cast<const struct rte_udp_hdr *>(frame + udp_offset);
    const uint16_t dst_port = rte_be_to_cpu_16(udp->dst_port);
    const uint16_t udp_len = rte_be_to_cpu_16(udp->dgram_len);
    if (udp_len < sizeof(struct rte_udp_hdr) ||
        udp_offset + static_cast<std::size_t>(udp_len) >
            ipv4_offset + static_cast<std::size_t>(total_len)) {
      return {ParseStatus::Malformed, nullptr, 0, 0, true};
    }

    uint8_t line_index = 0;
    if (line_a_.matches(ipv4->dst_addr, dst_port)) {
      line_index = 0;
    } else if (line_b_.matches(ipv4->dst_addr, dst_port)) {
      line_index = 1;
    } else {
      return {ParseStatus::Filtered, nullptr, 0, 0, true};
    }

    return {ParseStatus::Accepted, frame + payload_offset,
            static_cast<std::size_t>(udp_len) - sizeof(struct rte_udp_hdr),
            line_index, true};
  }

  ParsedPacket parseGeneral(const std::byte *frame,
                            std::size_t frame_len) const noexcept {
    std::size_t offset = 0;

    if (frame_len < sizeof(struct rte_ether_hdr))
      return {ParseStatus::Malformed};

    const auto *ether =
        reinterpret_cast<const struct rte_ether_hdr *>(frame + offset);
    offset += sizeof(struct rte_ether_hdr);
    uint16_t ether_type = rte_be_to_cpu_16(ether->ether_type);

    for (int depth = 0; depth < 2 && isVlanEtherType(ether_type); ++depth) {
      if (offset + sizeof(struct rte_vlan_hdr) > frame_len)
        return {ParseStatus::Malformed};
      const auto *vlan =
          reinterpret_cast<const struct rte_vlan_hdr *>(frame + offset);
      offset += sizeof(struct rte_vlan_hdr);
      ether_type = rte_be_to_cpu_16(vlan->eth_proto);
    }

    if (ether_type != RTE_ETHER_TYPE_IPV4)
      return {ParseStatus::Filtered};

    if (offset + sizeof(struct rte_ipv4_hdr) > frame_len)
      return {ParseStatus::Malformed};

    const auto *ipv4 =
        reinterpret_cast<const struct rte_ipv4_hdr *>(frame + offset);
    const uint8_t version = static_cast<uint8_t>(ipv4->version_ihl >> 4);
    const std::size_t ipv4_header_len =
        static_cast<std::size_t>(ipv4->version_ihl & 0x0F) * 4;
    if (version != 4 || ipv4_header_len < sizeof(struct rte_ipv4_hdr))
      return {ParseStatus::Malformed};
    if (offset + ipv4_header_len > frame_len)
      return {ParseStatus::Malformed};

    const uint16_t total_len = rte_be_to_cpu_16(ipv4->total_length);
    if (total_len < ipv4_header_len ||
        offset + static_cast<std::size_t>(total_len) > frame_len)
      return {ParseStatus::Malformed};

    const uint16_t fragment_offset = rte_be_to_cpu_16(ipv4->fragment_offset);
    if ((fragment_offset &
         (kIpv4FragmentOffsetMask | kIpv4MoreFragmentsFlag)) != 0)
      return {ParseStatus::Malformed};

    if (ipv4->next_proto_id != IPPROTO_UDP)
      return {ParseStatus::Filtered};

    const std::size_t udp_offset = offset + ipv4_header_len;
    if (udp_offset + sizeof(struct rte_udp_hdr) > frame_len)
      return {ParseStatus::Malformed};

    const auto *udp =
        reinterpret_cast<const struct rte_udp_hdr *>(frame + udp_offset);
    const uint16_t dst_port = rte_be_to_cpu_16(udp->dst_port);
    const uint16_t udp_len = rte_be_to_cpu_16(udp->dgram_len);
    if (udp_len < sizeof(struct rte_udp_hdr))
      return {ParseStatus::Malformed};
    if (udp_offset + static_cast<std::size_t>(udp_len) >
        offset + static_cast<std::size_t>(total_len))
      return {ParseStatus::Malformed};

    uint8_t line_index = 0;
    if (line_a_.matches(ipv4->dst_addr, dst_port)) {
      line_index = 0;
    } else if (line_b_.matches(ipv4->dst_addr, dst_port)) {
      line_index = 1;
    } else {
      return {ParseStatus::Filtered};
    }

    const std::size_t payload_offset = udp_offset + sizeof(struct rte_udp_hdr);
    return {ParseStatus::Accepted, frame + payload_offset,
            static_cast<std::size_t>(udp_len) - sizeof(struct rte_udp_hdr),
            line_index};
  }

  struct rte_eth_stats ethStats() const noexcept {
    struct rte_eth_stats stats {};
    if (rte_eth_stats_get(port_id_, &stats) != 0)
      return {};
    return stats;
  }

  void releaseDelivered() noexcept {
    if (delivered_mbuf_ != nullptr) {
      rte_pktmbuf_free(delivered_mbuf_);
      delivered_mbuf_ = nullptr;
    }
  }

  void releaseCachedBurst() noexcept {
    while (burst_index_ < burst_count_) {
      rte_pktmbuf_free(burst_[burst_index_++]);
    }
    burst_index_ = 0;
    burst_count_ = 0;
  }
};

DpdkReceiver::DpdkReceiver(const char *ip, uint16_t port)
    : impl_(std::make_unique<Impl>(ip, port)) {}

DpdkReceiver::DpdkReceiver(const char *ip_a, uint16_t port_a, const char *ip_b,
                           uint16_t port_b)
    : impl_(std::make_unique<Impl>(ip_a, port_a, ip_b, port_b)) {}

DpdkReceiver::~DpdkReceiver() = default;

bool DpdkReceiver::next(PacketView &packet) {
  return impl_->next(packet);
}

uint64_t DpdkReceiver::packetsA() const noexcept {
  return impl_->packets_a_;
}

uint64_t DpdkReceiver::packetsB() const noexcept {
  return impl_->packets_b_;
}

uint64_t DpdkReceiver::filtered() const noexcept {
  return impl_->filtered_count_;
}

uint64_t DpdkReceiver::malformed() const noexcept {
  return impl_->malformed_count_;
}

uint64_t DpdkReceiver::fastPathPackets() const noexcept {
  return impl_->fast_path_count_;
}

uint64_t DpdkReceiver::fallbackPathPackets() const noexcept {
  return impl_->fallback_path_count_;
}

uint64_t DpdkReceiver::polls() const noexcept { return impl_->poll_count_; }

uint64_t DpdkReceiver::emptyPolls() const noexcept {
  return impl_->empty_poll_count_;
}

uint64_t DpdkReceiver::imissed() const noexcept { return impl_->imissed(); }

uint64_t DpdkReceiver::ierrors() const noexcept { return impl_->ierrors(); }

uint64_t DpdkReceiver::rxNombuf() const noexcept {
  return impl_->rxNombuf();
}

std::size_t DpdkReceiver::burstSize() const noexcept {
  return impl_->burst_size_;
}

uint16_t DpdkReceiver::portId() const noexcept { return impl_->port_id_; }

uint16_t DpdkReceiver::queueId() const noexcept { return impl_->queue_id_; }
