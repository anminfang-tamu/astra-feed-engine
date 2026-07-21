#include "astra/book/BookManager.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/engine/EngineConfig.hpp"
#include "astra/engine/MarketDataEngine.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
// #include "astra/metrics/StageLatencyRecorder.hpp"
#ifdef ASTRA_ENABLE_DPDK
#include "astra/source/DpdkReceiver.hpp"
#endif
#include "astra/source/DualUdpBatchReceiver.hpp"
#include "astra/source/DualUdpReceiver.hpp"
#include "astra/source/UdpBatchReceiver.hpp"
#include "astra/source/UdpReceiver.hpp"
#include "astra/symbol/StockDirectory.hpp"
#include "astra/utils/CpuAffinity.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

static MarketDataEngine *g_engine = nullptr;

static void onSignal(int) {
  if (g_engine)
    g_engine->stop();
}

static bool envBool(const char *value, bool fallback) noexcept {
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

static std::size_t envSize(const char *name, std::size_t fallback,
                           std::size_t minimum,
                           std::size_t maximum =
                               std::numeric_limits<std::size_t>::max()) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }

  errno = 0;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum ||
      parsed > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error(std::string(name) + " must be an integer in [" +
                             std::to_string(minimum) + ", " +
                             std::to_string(maximum) + "]");
  }
  return static_cast<std::size_t>(parsed);
}

static const char *channelHealthName(ChannelHealth status) noexcept {
  switch (status) {
  case ChannelHealth::Good:
    return "Good";
  case ChannelHealth::GapDetected:
    return "GapDetected";
  case ChannelHealth::Recovering:
    return "Recovering";
  case ChannelHealth::Stale:
    return "Stale";
  case ChannelHealth::Invalid:
    return "Invalid";
  }
  return "Unknown";
}

static bool bookStatsClean(const BookManagerStats &stats) noexcept {
  return stats.invalid_adds == 0 && stats.duplicate_order_refs == 0 &&
         stats.missing_order_refs == 0 && stats.stale_order_refs == 0 &&
         stats.mutation_failures == 0 &&
         stats.order_ref_index_insert_failures == 0 &&
         stats.order_ref_index_replace_failures == 0 &&
         stats.order_ref_index_erase_failures == 0 &&
         stats.late_book_creation_attempts == 0 &&
         stats.book_creation_failures == 0 &&
         stats.allocation_failures == 0 && stats.price_rejections == 0 &&
         stats.locally_invalid_books == 0 &&
         stats.inconsistent_books == 0 &&
         stats.orders.invalid_releases == 0 &&
         stats.orders.double_releases == 0 &&
         stats.price_levels.internal_node_exhaustions == 0 &&
         stats.price_levels.leaf_exhaustions == 0 &&
         stats.price_levels.level_exhaustions == 0 &&
         stats.order_ref_index_size == stats.live_orders &&
         stats.orders.in_use == stats.live_orders;
}

static bool channelStatsClean(const ChannelState &channel,
                              bool dual_feed) noexcept {
  return channel.status == ChannelHealth::Good &&
         channel.gap_buffer.empty() && channel.session_initialized &&
         channel.first_received_seq == 1 &&
         channel.first_received_seq_by_line[0] == 1 &&
         (!dual_feed || channel.first_received_seq_by_line[1] == 1) &&
         channel.session_mismatch_packets == 0;
}

int main(int argc, char *argv[]) {
  if (argc != 3 && argc != 4 && argc != 5 && argc != 6) {
    std::cerr
        << "Usage:\n"
        << "  " << argv[0] << " <ip> <port> [channel_id]\n"
        << "  " << argv[0] << " <ip_a> <port_a> <ip_b> <port_b> [channel_id]\n"
        << "  Receives MoldUDP64/ITCH 5.0 and builds a live order book.\n";
    return EXIT_FAILURE;
  }

  const bool dual_feed = argc == 5 || argc == 6;
  const char *ip = argv[1];
  const uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
  const uint8_t channel_id =
      (!dual_feed && argc == 4)
          ? static_cast<uint8_t>(std::strtoul(argv[3], nullptr, 10))
      : (dual_feed && argc == 6)
          ? static_cast<uint8_t>(std::strtoul(argv[5], nullptr, 10))
          : 0;

  try {
    if (const char *cpu_env = std::getenv("ASTRA_CPU")) {
      const int cpu_id = static_cast<int>(std::strtol(cpu_env, nullptr, 10));
      const auto affinity = astra::utils::pinCurrentThreadToCpu(cpu_id);
      std::cout << "cpu_affinity status="
                << astra::utils::cpuAffinityStatusName(affinity.status)
                << " cpu=" << affinity.cpu_id;
      if (!affinity)
        std::cout << " error=" << affinity.error_code << " message=\""
                  << affinity.message << "\"";
      std::cout << "\n";
    }

    const char *rx_transport = std::getenv("ASTRA_RX");
    const bool use_dpdk =
        rx_transport != nullptr && std::strcmp(rx_transport, "dpdk") == 0;
    if (rx_transport != nullptr && !use_dpdk &&
        std::strcmp(rx_transport, "udp") != 0 &&
        std::strcmp(rx_transport, "kernel") != 0) {
      throw std::runtime_error(std::string("Unknown ASTRA_RX: ") +
                               rx_transport);
    }

    const char *rx_mode = std::getenv("ASTRA_UDP_RX");
    const bool use_recvmmsg =
        rx_mode != nullptr && (std::strcmp(rx_mode, "recvmmsg") == 0 ||
                               std::strcmp(rx_mode, "batch") == 0);
    std::size_t batch_size = UdpBatchReceiver::kDefaultBatchSize;
    if (const char *batch_env = std::getenv("ASTRA_UDP_BATCH_SIZE")) {
      const auto parsed = std::strtoul(batch_env, nullptr, 10);
      if (parsed > 0)
        batch_size = static_cast<std::size_t>(parsed);
    }

    const std::size_t default_book_order_capacity = envSize(
        "ASTRA_BOOK_ORDER_CAPACITY",
        astra::book_capacity::kDefaultOrderCapacity, 1,
        static_cast<std::size_t>(OrderArena::kInvalidIndex));
    PriceLevelArenaConfig price_level_config{
        BookManager::kDefaultPriceInternalNodeCapacity,
        BookManager::kDefaultPriceLeafCapacity,
        BookManager::kDefaultPriceLevelCapacity};
    price_level_config.internal_node_capacity = envSize(
        "ASTRA_PRICE_INTERNAL_NODE_CAPACITY",
        price_level_config.internal_node_capacity, 2);
    price_level_config.leaf_capacity =
        envSize("ASTRA_PRICE_LEAF_CAPACITY", price_level_config.leaf_capacity,
                1);
    price_level_config.level_capacity = envSize(
        "ASTRA_PRICE_LEVEL_CAPACITY", price_level_config.level_capacity, 1,
        Order::kLevelMask);

    astra::symbol::StockDirectory symbols;
    BookManager book_manager(default_book_order_capacity, price_level_config);
    MoldUdpDecoder decoder(symbols, book_manager, channel_id);
    std::unique_ptr<IMarketDataSource> source;
    DualUdpReceiver *dual_receiver = nullptr;
    DualUdpBatchReceiver *dual_batch_receiver = nullptr;
    UdpBatchReceiver *batch_receiver = nullptr;
#ifdef ASTRA_ENABLE_DPDK
    DpdkReceiver *dpdk_receiver = nullptr;
#endif
    if (use_dpdk) {
#ifndef ASTRA_ENABLE_DPDK
      throw std::runtime_error(
          "ASTRA_RX=dpdk requested, but md_engine was built without "
          "-DASTRA_ENABLE_DPDK=ON");
#else
      if (dual_feed) {
        auto receiver = std::make_unique<DpdkReceiver>(
            argv[1], static_cast<uint16_t>(std::atoi(argv[2])), argv[3],
            static_cast<uint16_t>(std::atoi(argv[4])));
        dpdk_receiver = receiver.get();
        source = std::move(receiver);
      } else {
        auto receiver = std::make_unique<DpdkReceiver>(ip, port);
        dpdk_receiver = receiver.get();
        source = std::move(receiver);
      }
#endif
    } else if (dual_feed) {
      if (use_recvmmsg) {
        auto receiver = std::make_unique<DualUdpBatchReceiver>(
            argv[1], static_cast<uint16_t>(std::atoi(argv[2])), argv[3],
            static_cast<uint16_t>(std::atoi(argv[4])), batch_size);
        dual_batch_receiver = receiver.get();
        source = std::move(receiver);
      } else {
        auto receiver = std::make_unique<DualUdpReceiver>(
            argv[1], static_cast<uint16_t>(std::atoi(argv[2])), argv[3],
            static_cast<uint16_t>(std::atoi(argv[4])));
        dual_receiver = receiver.get();
        source = std::move(receiver);
      }
    } else if (use_recvmmsg) {
      auto receiver = std::make_unique<UdpBatchReceiver>(ip, port, batch_size);
      batch_receiver = receiver.get();
      source = std::move(receiver);
    } else {
      source = std::make_unique<UdpReceiver>(ip, port);
    }
    LatencyRecorder latency_recorder;
    // StageLatencyRecorder stage_latency_recorder;
    EngineConfig config;
    config.enable_latency_metrics = envBool(
        std::getenv("ASTRA_LATENCY_METRICS"), config.enable_latency_metrics);
    config.stop_on_decode_error = envBool(
        std::getenv("ASTRA_STOP_ON_DECODE_ERROR"), true);
    // config.enable_stage_latency_metrics =
    //     config.enable_latency_metrics &&
    //     envBool(std::getenv("ASTRA_STAGE_LATENCY_METRICS"),
    //             config.enable_stage_latency_metrics);
    // decoder.setStageTimingEnabled(config.enable_latency_metrics &&
    //                               config.enable_stage_latency_metrics);

    MarketDataEngine engine(*source, decoder, latency_recorder, config);

    g_engine = &engine;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const char *rx_name =
        use_dpdk ? "dpdk" : (use_recvmmsg ? "recvmmsg" : "recv");
    std::size_t rx_batch_size = use_recvmmsg ? batch_size : 1;
#ifdef ASTRA_ENABLE_DPDK
    if (dpdk_receiver != nullptr)
      rx_batch_size = dpdk_receiver->burstSize();
#endif

    if (dual_feed) {
      std::cout << "Engine started  line_a=" << argv[1] << ":" << argv[2]
                << "  line_b=" << argv[3] << ":" << argv[4]
                << "  channel=" << static_cast<int>(channel_id)
                << "  rx=" << rx_name << "  batch_size=" << rx_batch_size;
#ifdef ASTRA_ENABLE_DPDK
      if (dpdk_receiver != nullptr) {
        std::cout << "  dpdk_port=" << dpdk_receiver->portId()
                  << "  dpdk_queue=" << dpdk_receiver->queueId();
      }
#endif
      std::cout << "  metrics="
                << (config.enable_latency_metrics ? "on" : "off")
                << "  stop_on_decode_error="
                << (config.stop_on_decode_error ? "on" : "off")
                << "  default_book_order_capacity="
                << default_book_order_capacity
                << "  price_internal_nodes="
                << price_level_config.internal_node_capacity
                << "  price_leaves="
                << price_level_config.leaf_capacity
                << "  price_levels="
                << price_level_config.level_capacity
                // << "  stage_metrics="
                // << (config.enable_stage_latency_metrics ? "on" : "off")
                << " — press Ctrl+C to stop\n";
    } else {
      std::cout << "Engine started  addr=" << ip << ":" << port
                << "  channel=" << static_cast<int>(channel_id)
                << "  rx=" << rx_name << "  batch_size=" << rx_batch_size;
#ifdef ASTRA_ENABLE_DPDK
      if (dpdk_receiver != nullptr) {
        std::cout << "  dpdk_port=" << dpdk_receiver->portId()
                  << "  dpdk_queue=" << dpdk_receiver->queueId();
      }
#endif
      std::cout << "  metrics="
                << (config.enable_latency_metrics ? "on" : "off")
                << "  stop_on_decode_error="
                << (config.stop_on_decode_error ? "on" : "off")
                << "  default_book_order_capacity="
                << default_book_order_capacity
                << "  price_internal_nodes="
                << price_level_config.internal_node_capacity
                << "  price_leaves="
                << price_level_config.leaf_capacity
                << "  price_levels="
                << price_level_config.level_capacity
                // << "  stage_metrics="
                // << (config.enable_stage_latency_metrics ? "on" : "off")
                << " — press Ctrl+C to stop\n";
    }

    engine.run();

    const auto &channel = decoder.channelState();

    std::cout << "Engine stopped  symbols=" << symbols.size() << "\n";
    std::cout << "engine_stats channel_next_seq=" << channel.next_expected_seq
              << " channel_first_received_seq="
              << channel.first_received_seq
              << " line_a_first_received_seq="
              << channel.first_received_seq_by_line[0]
              << " line_b_first_received_seq="
              << channel.first_received_seq_by_line[1]
              << " session_initialized="
              << (channel.session_initialized ? 1 : 0)
              << " session_mismatch_packets="
              << channel.session_mismatch_packets
              << " gap_buffer_remaining=" << channel.gap_buffer.size()
              << " channel_status=" << static_cast<int>(channel.status)
              << " channel_status_name=" << channelHealthName(channel.status)
              << "\n";

    const BookManagerStats book_stats = book_manager.stats();
    std::cout << "book_stats books=" << book_stats.books
              << " live_orders=" << book_stats.live_orders
              << " local_index_size=" << book_stats.order_ref_index_size
              << " local_index_slots=" << book_stats.order_ref_index_capacity
              << " local_index_max_entries="
              << book_stats.order_ref_index_max_entries
              << " local_index_max_probe="
              << book_stats.order_ref_index_max_probe_length
              << " invalid_adds=" << book_stats.invalid_adds
              << " duplicate_order_refs="
              << book_stats.duplicate_order_refs
              << " missing_order_refs=" << book_stats.missing_order_refs
              << " stale_order_refs=" << book_stats.stale_order_refs
              << " mutation_failures=" << book_stats.mutation_failures
              << " order_ref_index_insert_failures="
              << book_stats.order_ref_index_insert_failures
              << " order_ref_index_replace_failures="
              << book_stats.order_ref_index_replace_failures
              << " order_ref_index_erase_failures="
              << book_stats.order_ref_index_erase_failures
              << " late_book_creation_attempts="
              << book_stats.late_book_creation_attempts
              << " book_creation_failures="
              << book_stats.book_creation_failures
              << " allocation_failures=" << book_stats.allocation_failures
              << " local_order_arenas_in_use=" << book_stats.orders.in_use
              << " local_order_arenas_capacity="
              << book_stats.orders.capacity
              << " live_order_high_watermark="
              << book_stats.orders.high_watermark
              << " local_order_arena_invalid_releases="
              << book_stats.orders.invalid_releases
              << " local_order_arena_double_releases="
              << book_stats.orders.double_releases
              << " price_rejections=" << book_stats.price_rejections
              << " locally_invalid_books="
              << book_stats.locally_invalid_books
              << " inconsistent_books=" << book_stats.inconsistent_books
              << " price_internal_nodes_in_use="
              << book_stats.price_levels.internal_nodes_in_use
              << " price_internal_nodes_high_watermark="
              << book_stats.price_levels.internal_node_high_watermark
              << " price_leaves_in_use="
              << book_stats.price_levels.leaves_in_use
              << " price_leaf_high_watermark="
              << book_stats.price_levels.leaf_high_watermark
              << " price_levels_in_use="
              << book_stats.price_levels.levels_in_use
              << " price_level_high_watermark="
              << book_stats.price_levels.level_high_watermark
              << " price_internal_node_exhaustions="
              << book_stats.price_levels.internal_node_exhaustions
              << " price_leaf_exhaustions="
              << book_stats.price_levels.leaf_exhaustions
              << " price_level_exhaustions="
              << book_stats.price_levels.level_exhaustions << "\n";

    if (channel.status != ChannelHealth::Good || !channel.gap_buffer.empty()) {
      const auto gap_stats = channel.gap_buffer.stats(channel.next_expected_seq);
      std::cout << "gap_stats missing_seq=" << channel.next_expected_seq
                << " buffered_packets=" << gap_stats.size
                << " min_buffered_seq=" << gap_stats.min_seq
                << " max_buffered_seq=" << gap_stats.max_seq
                << " next_buffered_seq=" << gap_stats.next_seq_at_or_after;
      if (gap_stats.next_seq_at_or_after >= channel.next_expected_seq &&
          gap_stats.next_seq_at_or_after != 0) {
        std::cout << " gap_messages_to_next="
                  << (gap_stats.next_seq_at_or_after -
                      channel.next_expected_seq);
      }
      std::cout << "\n";
    }

    if (batch_receiver != nullptr) {
      std::cout << "rx_stats syscalls=" << batch_receiver->syscalls()
                << " errors=" << batch_receiver->errors()
                << " truncated=" << batch_receiver->truncated()
                << " kernel_drops=" << batch_receiver->kernelDrops() << "\n";
    }
    if (dual_receiver != nullptr) {
      std::cout << "rx_stats line_a_packets=" << dual_receiver->packetsA()
                << " line_b_packets=" << dual_receiver->packetsB()
                << " line_a_errors=" << dual_receiver->errorsA()
                << " line_b_errors=" << dual_receiver->errorsB()
                << " line_a_truncated=" << dual_receiver->truncatedA()
                << " line_b_truncated=" << dual_receiver->truncatedB()
                << " drop_metrics="
                << (dual_receiver->dropMetricsEnabled() ? "on" : "off")
                << " line_a_kernel_drops=" << dual_receiver->kernelDropsA()
                << " line_b_kernel_drops=" << dual_receiver->kernelDropsB()
                << "\n";
    }
    if (dual_batch_receiver != nullptr) {
      std::cout << "rx_stats line_a_packets=" << dual_batch_receiver->packetsA()
                << " line_b_packets=" << dual_batch_receiver->packetsB()
                << " line_a_syscalls=" << dual_batch_receiver->syscallsA()
                << " line_b_syscalls=" << dual_batch_receiver->syscallsB()
                << " line_a_errors=" << dual_batch_receiver->errorsA()
                << " line_b_errors=" << dual_batch_receiver->errorsB()
                << " line_a_truncated=" << dual_batch_receiver->truncatedA()
                << " line_b_truncated=" << dual_batch_receiver->truncatedB()
                << " line_a_kernel_drops="
                << dual_batch_receiver->kernelDropsA()
                << " line_b_kernel_drops="
                << dual_batch_receiver->kernelDropsB() << "\n";
    }
#ifdef ASTRA_ENABLE_DPDK
    if (dpdk_receiver != nullptr) {
      std::cout << "rx_stats line_a_packets=" << dpdk_receiver->packetsA()
                << " line_b_packets=" << dpdk_receiver->packetsB()
                << " filtered=" << dpdk_receiver->filtered()
                << " malformed=" << dpdk_receiver->malformed()
                << " fast_path=" << dpdk_receiver->fastPathPackets()
                << " fallback_path=" << dpdk_receiver->fallbackPathPackets()
                << " polls=" << dpdk_receiver->polls()
                << " empty_polls=" << dpdk_receiver->emptyPolls()
                << " imissed=" << dpdk_receiver->imissed()
                << " ierrors=" << dpdk_receiver->ierrors()
                << " rx_nombuf=" << dpdk_receiver->rxNombuf() << "\n";
    }
#endif
    if (config.enable_latency_metrics) {
      latency_recorder.report();
      // if (config.enable_stage_latency_metrics)
      //   stage_latency_recorder.report();
    }

    if (!channelStatsClean(channel, dual_feed) ||
        !bookStatsClean(book_stats)) {
      return EXIT_FAILURE;
    }

  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
