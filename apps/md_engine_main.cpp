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

#include <charconv>
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

static void pinRequestedCpu(const char *phase) {
  const char *cpu_env = std::getenv("ASTRA_CPU");
  if (cpu_env == nullptr || *cpu_env == '\0')
    return;

  const char *end = cpu_env + std::strlen(cpu_env);
  uint64_t parsed_cpu = 0;
  const auto parse_result =
      std::from_chars(cpu_env, end, parsed_cpu, 10);
  if (parse_result.ec != std::errc{} || parse_result.ptr != end ||
      parsed_cpu >
          static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument(
        "ASTRA_CPU must be a non-negative in-range integer CPU ID");
  }

  const int cpu_id = static_cast<int>(parsed_cpu);
  const auto affinity = astra::utils::pinCurrentThreadToCpu(cpu_id);
  std::cout << "cpu_affinity status="
            << astra::utils::cpuAffinityStatusName(affinity.status)
            << " cpu=" << affinity.cpu_id << " phase=" << phase;
  if (!affinity)
    std::cout << " error=" << affinity.error_code << " message=\""
              << affinity.message << "\"";
  std::cout << "\n";
  if (!affinity)
    throw std::runtime_error(
        "requested CPU affinity is required for this run");
}

static bool envBool(const char *name, const char *value, bool fallback) {
  if (value == nullptr)
    return fallback;
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
      std::strcmp(value, "off") == 0 || std::strcmp(value, "no") == 0)
    return false;
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
      std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0)
    return true;
  throw std::invalid_argument(std::string(name) +
                              " must be one of on/off, true/false, yes/no, 1/0");
}

static uint64_t envU64(const char *name, uint64_t fallback,
                       uint64_t minimum,
                       uint64_t maximum) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    return fallback;
  const char *end = value + std::strlen(value);
  uint64_t parsed = 0;
  const auto result = std::from_chars(value, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end || parsed < minimum ||
      parsed > maximum) {
    throw std::invalid_argument(std::string(name) +
                                " is not a valid in-range integer");
  }
  return parsed;
}

static uint64_t requiredEnvU64(const char *name, uint64_t minimum,
                               uint64_t maximum) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    throw std::invalid_argument(std::string(name) +
                                " is required for a custom capacity profile");
  }
  return envU64(name, 0, minimum, maximum);
}

static bool parseUnsignedArgument(const char *value, uint64_t minimum,
                                  uint64_t maximum,
                                  uint64_t &parsed) noexcept {
  if (value == nullptr || *value == '\0')
    return false;
  const char *end = value + std::strlen(value);
  parsed = 0;
  const auto result = std::from_chars(value, end, parsed, 10);
  return result.ec == std::errc{} && result.ptr == end &&
         parsed >= minimum && parsed <= maximum;
}

static std::string requiredMetadataToken(const char *name,
                                         std::size_t maximum_length) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::invalid_argument(std::string(name) + " is required");

  const std::string token(value);
  if (token.size() > maximum_length) {
    throw std::invalid_argument(std::string(name) + " is too long");
  }
  for (const unsigned char character : token) {
    const bool ascii_alphanumeric =
        (character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z');
    if (!ascii_alphanumeric && character != '-' && character != '_' &&
        character != '.' && character != ':' && character != '/' &&
        character != '+') {
      throw std::invalid_argument(
          std::string(name) +
          " must contain only alphanumerics or - _ . : / +");
    }
  }
  return token;
}

static std::string requiredSha256(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::invalid_argument(std::string(name) + " is required");
  const std::string digest(value);
  if (digest.size() != 64) {
    throw std::invalid_argument(std::string(name) +
                                " must be exactly 64 lowercase hex digits");
  }
  for (const char character : digest) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      throw std::invalid_argument(std::string(name) +
                                  " must be exactly 64 lowercase hex digits");
    }
  }
  return digest;
}

struct BookDeploymentCapacity {
  std::string name;
  std::string evidence_schema;
  std::string evidence_sha256;
  std::string corpus_manifest_sha256;
  std::string profiler_sha256;
  std::string profile_output_sha256;
  BookManagerConfig config;
  BookCapacityEvidence evidence;
  BookCapacityHeadroom minimum_headroom;
  BookCapacityHeadroom effective_headroom;
};

static std::string requiredEnvironmentValue(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::invalid_argument(std::string(name) + " is required");
  return value;
}

static void requireManifestValue(const char *name, std::uint64_t configured,
                                 std::uint64_t manifested) {
  if (configured != manifested) {
    throw std::invalid_argument(std::string(name) +
                                " differs from the capacity evidence manifest");
  }
}

static BookDeploymentCapacity bookCapacityFromEnvironment() {
  const std::string profile_name =
      requiredMetadataToken("ASTRA_BOOK_CAPACITY_PROFILE", 128);

  const std::string expected_evidence_sha256 =
      requiredSha256("ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256");
  const std::string evidence_file =
      requiredEnvironmentValue("ASTRA_BOOK_CAPACITY_EVIDENCE_FILE");
  const std::uint64_t configured_direct_slots = requiredEnvU64(
      "ASTRA_ORDER_DIRECT_SLOTS", 1,
      static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()));
  const std::uint64_t configured_fallback_buckets = requiredEnvU64(
      "ASTRA_ORDER_FALLBACK_BUCKETS", 1,
      std::numeric_limits<uint32_t>::max());
  const std::uint64_t configured_price_pages = requiredEnvU64(
      "ASTRA_PRICE_PAGE_CAPACITY", 1,
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 1);
  const std::uint64_t configured_maximum_order_reference = requiredEnvU64(
      "ASTRA_PROFILED_MAX_ORDER_REF", 1,
      std::numeric_limits<uint64_t>::max());
  const std::uint64_t configured_unique_price_pages = requiredEnvU64(
      "ASTRA_PROFILED_UNIQUE_PRICE_PAGES", 1,
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 1);
  const std::uint64_t configured_minimum_direct_headroom = requiredEnvU64(
      "ASTRA_MIN_DIRECT_ORDER_HEADROOM", 1,
      std::numeric_limits<uint64_t>::max());
  const std::uint64_t configured_minimum_price_headroom = requiredEnvU64(
      "ASTRA_MIN_PRICE_PAGE_HEADROOM", 1,
      std::numeric_limits<uint32_t>::max());
  BookCapacityEvidenceManifest manifest =
      loadBookCapacityEvidenceManifest(evidence_file, profile_name,
                                       expected_evidence_sha256);
  if (manifest.schema != BookCapacityEvidenceManifest::kSchemaV2) {
    throw std::invalid_argument(
        "md_engine startup requires astra_book_capacity_evidence_v2");
  }
  BookCapacityProfile profile = std::move(manifest.profile);

  requireManifestValue("ASTRA_ORDER_DIRECT_SLOTS",
                       configured_direct_slots,
                       profile.config.orders.direct_slots);
  requireManifestValue("ASTRA_ORDER_FALLBACK_BUCKETS",
                       configured_fallback_buckets,
                       profile.config.orders.fallback_bucket_count);
  requireManifestValue("ASTRA_PRICE_PAGE_CAPACITY",
                       configured_price_pages,
                       profile.config.prices.page_capacity);
  requireManifestValue(
      "ASTRA_PROFILED_MAX_ORDER_REF",
      configured_maximum_order_reference,
      profile.evidence.profiled_max_order_reference);
  requireManifestValue(
      "ASTRA_PROFILED_UNIQUE_PRICE_PAGES",
      configured_unique_price_pages,
      profile.evidence.profiled_unique_price_pages);
  requireManifestValue(
      "ASTRA_MIN_DIRECT_ORDER_HEADROOM",
      configured_minimum_direct_headroom,
      profile.minimum_headroom.direct_order_reference_slots);
  requireManifestValue("ASTRA_MIN_PRICE_PAGE_HEADROOM",
                       configured_minimum_price_headroom,
                       profile.minimum_headroom.price_pages);

  const bool prefault = envBool("ASTRA_BOOK_PREFAULT",
                               std::getenv("ASTRA_BOOK_PREFAULT"), true);
  profile.config.orders.prefault = prefault;
  profile.config.prices.prefault = prefault;

  const BookCapacityValidation validation =
      validateBookCapacityProfile(profile);
  return {std::string(profile.name),
          std::move(manifest.schema),
          std::string(profile.evidence_sha256),
          std::move(manifest.corpus_manifest_sha256),
          std::move(manifest.profiler_sha256),
          std::move(manifest.profile_output_sha256),
          profile.config,
          profile.evidence,
          profile.minimum_headroom,
          validation.effective_headroom};
}

static void printBookCapacityProfile(
    const BookDeploymentCapacity &deployment) {
  std::cout << "book_capacity_profile name=" << deployment.name
            << " evidence_schema=" << deployment.evidence_schema
            << " evidence_sha256=" << deployment.evidence_sha256
            << " corpus_manifest_sha256="
            << deployment.corpus_manifest_sha256
            << " profiler_sha256=" << deployment.profiler_sha256
            << " profile_output_sha256="
            << (deployment.profile_output_sha256.empty()
                    ? "not-recorded-v1"
                    : deployment.profile_output_sha256)
            << " order_direct_slots="
            << deployment.config.orders.direct_slots
            << " order_fallback_buckets="
            << deployment.config.orders.fallback_bucket_count
            << " price_page_capacity="
            << deployment.config.prices.page_capacity
            << " profiled_max_order_ref="
            << deployment.evidence.profiled_max_order_reference
            << " profiled_unique_price_pages="
            << deployment.evidence.profiled_unique_price_pages
            << " minimum_direct_order_headroom="
            << deployment.minimum_headroom.direct_order_reference_slots
            << " effective_direct_order_headroom="
            << deployment.effective_headroom.direct_order_reference_slots
            << " minimum_price_page_headroom="
            << deployment.minimum_headroom.price_pages
            << " effective_price_page_headroom="
            << deployment.effective_headroom.price_pages << "\n";
}

static void printBookStoragePlan(const BookStorageFootprint &planned_storage) {
  std::cout << "book_storage_plan system_page_bytes="
            << planned_storage.system_page_bytes
            << " planned_mapped_bytes="
            << planned_storage.mapped_array_bytes
            << " planned_descriptor_bytes="
            << planned_storage.descriptor_slot_bytes
            << " planned_descriptor_mapped_bytes="
            << planned_storage.descriptor_mapped_bytes
            << " planned_storage_bytes="
            << planned_storage.planned_storage_bytes << "\n"
            << std::flush;
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

static const char *channelPhaseName(ChannelPhase phase) noexcept {
  switch (phase) {
  case ChannelPhase::WaitingStartOfMessages:
    return "WaitingStartOfMessages";
  case ChannelPhase::StartupDirectorySpin:
    return "StartupDirectorySpin";
  case ChannelPhase::StartupAdminSpin:
    return "StartupAdminSpin";
  case ChannelPhase::SystemHours:
    return "SystemHours";
  case ChannelPhase::MarketHours:
    return "MarketHours";
  case ChannelPhase::PostMarketHours:
    return "PostMarketHours";
  case ChannelPhase::PostSystemHours:
    return "PostSystemHours";
  case ChannelPhase::EndOfMessages:
    return "EndOfMessages";
  }
  return "Unknown";
}

int main(int argc, char *argv[]) {
  if (argc == 2 &&
      std::strcmp(argv[1], "--book-storage-plan-only") == 0) {
    try {
      const BookDeploymentCapacity deployment =
          bookCapacityFromEnvironment();
      const BookStorageFootprint planned_storage =
          estimateBookStorageFootprint(deployment.config);
      printBookCapacityProfile(deployment);
      printBookStoragePlan(planned_storage);
      return EXIT_SUCCESS;
    } catch (const std::exception &error) {
      std::cerr << "Fatal: " << error.what() << "\n";
      return EXIT_FAILURE;
    }
  }

  if (argc != 3 && argc != 4 && argc != 5 && argc != 6) {
    std::cerr
        << "Usage:\n"
        << "  " << argv[0] << " <ip> <port> [channel_id]\n"
        << "  " << argv[0] << " <ip_a> <port_a> <ip_b> <port_b> [channel_id]\n"
        << "  " << argv[0] << " --book-storage-plan-only\n"
        << "  Receives MoldUDP64/ITCH 5.0 and builds a live order book.\n";
    return EXIT_FAILURE;
  }

  const bool dual_feed = argc == 5 || argc == 6;
  const char *ip = argv[1];
  uint64_t parsed_argument = 0;
  if (!parseUnsignedArgument(
          argv[2], 1, std::numeric_limits<uint16_t>::max(),
          parsed_argument)) {
    std::cerr << "port_a must be an integer from 1 through "
              << std::numeric_limits<uint16_t>::max() << "\n";
    return EXIT_FAILURE;
  }
  const uint16_t port = static_cast<uint16_t>(parsed_argument);

  uint16_t port_b = 0;
  if (dual_feed &&
      !parseUnsignedArgument(
          argv[4], 1, std::numeric_limits<uint16_t>::max(),
          parsed_argument)) {
    std::cerr << "port_b must be an integer from 1 through "
              << std::numeric_limits<uint16_t>::max() << "\n";
    return EXIT_FAILURE;
  }
  if (dual_feed)
    port_b = static_cast<uint16_t>(parsed_argument);

  uint8_t channel_id = 0;
  const char *channel_argument =
      (!dual_feed && argc == 4) ? argv[3]
      : (dual_feed && argc == 6) ? argv[5]
                                 : nullptr;
  if (channel_argument != nullptr) {
    if (!parseUnsignedArgument(
            channel_argument, 0, std::numeric_limits<uint8_t>::max(),
            parsed_argument)) {
      std::cerr << "channel_id must be an integer from 0 through "
                << static_cast<unsigned>(
                       std::numeric_limits<uint8_t>::max())
                << "\n";
      return EXIT_FAILURE;
    }
    channel_id = static_cast<uint8_t>(parsed_argument);
  }

  try {
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
    if (rx_mode != nullptr && rx_mode[0] != '\0' && !use_recvmmsg &&
        std::strcmp(rx_mode, "recv") != 0) {
      throw std::runtime_error(std::string("Unknown ASTRA_UDP_RX: ") +
                               rx_mode);
    }
    std::size_t batch_size = UdpBatchReceiver::kDefaultBatchSize;
    if (std::getenv("ASTRA_UDP_BATCH_SIZE") != nullptr) {
      batch_size = static_cast<std::size_t>(
          envU64("ASTRA_UDP_BATCH_SIZE",
                 UdpBatchReceiver::kDefaultBatchSize, 1,
                 UdpBatchReceiver::kMaxBatchSize));
    }
    EngineConfig config;
    config.enable_latency_metrics = envBool(
        "ASTRA_LATENCY_METRICS", std::getenv("ASTRA_LATENCY_METRICS"),
        config.enable_latency_metrics);

    // Capacity provenance is a pure startup gate. Validate and report it
    // before binding a kernel socket or initializing a DPDK device.
    const BookDeploymentCapacity deployment = bookCapacityFromEnvironment();
    const BookManagerConfig &book_config = deployment.config;
    const BookStorageFootprint planned_storage =
        estimateBookStorageFootprint(book_config);
    printBookCapacityProfile(deployment);
    printBookStoragePlan(planned_storage);

    // DPDK EAL may replace the calling thread's affinity. Kernel receivers do
    // not, so pin them before opening sockets; the DPDK path is pinned again
    // immediately after EAL/NIC initialization and before large arena maps.
    if (!use_dpdk)
      pinRequestedCpu("pre_source");

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
            argv[1], port, argv[3], port_b);
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
            argv[1], port, argv[3], port_b, batch_size);
        dual_batch_receiver = receiver.get();
        source = std::move(receiver);
      } else {
        auto receiver = std::make_unique<DualUdpReceiver>(
            argv[1], port, argv[3], port_b);
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

    if (use_dpdk)
      pinRequestedCpu("post_dpdk_eal");

    astra::symbol::StockDirectory symbols;
    BookManager book_manager(book_config);
    const BookManagerConfig &effective_config = book_manager.config();
    const BookStorageFootprint &effective_storage =
        book_manager.storageFootprint();
    std::cout << "book_storage direct_order_slots="
              << effective_config.orders.direct_slots
              << " fallback_buckets="
              << effective_config.orders.fallback_bucket_count
              << " price_pages=" << effective_config.prices.page_capacity
              << " prefault="
              << (effective_config.prices.prefault ? "on" : "off")
              << " effective_mapped_bytes="
              << effective_storage.mapped_array_bytes
              << " effective_descriptor_bytes="
              << effective_storage.descriptor_slot_bytes
              << " effective_descriptor_mapped_bytes="
              << effective_storage.descriptor_mapped_bytes
              << " effective_storage_bytes="
              << effective_storage.planned_storage_bytes
              << "\n";
    MoldUdpDecoder decoder(symbols, book_manager, channel_id);

    LatencyRecorder latency_recorder;
    // StageLatencyRecorder stage_latency_recorder;
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
                // << "  stage_metrics="
                // << (config.enable_stage_latency_metrics ? "on" : "off")
                << " — press Ctrl+C to stop\n";
    }
    std::cout << std::flush;

    engine.run();

    const auto &channel = decoder.channelState();
    const std::uint64_t final_live_orders =
        book_manager.totalLiveOrderCount();
    const bool end_of_stream_accepted = decoder.endOfStreamAccepted();
    const BookDirectoryAudit book_directory_audit =
        book_manager.auditDirectory(symbols);
    const astra::book::PriceLevelStoreCounters price_counters =
        book_manager.priceLevels().counters();

    std::cout << "Engine stopped  symbols=" << symbols.size() << "\n";
    std::cout << "engine_stats channel_next_seq=" << channel.next_expected_seq
              << " channel_status=" << static_cast<int>(channel.status)
              << " channel_status_name=" << channelHealthName(channel.status)
              << " channel_phase_name=" << channelPhaseName(channel.phase)
              << " identical_buffered_redundant_packets="
              << channel.identical_buffered_redundant_packets
              << " conflicting_buffered_redundant_packets="
              << channel.conflicting_buffered_redundant_packets
              << " final_live_orders=" << final_live_orders
              << " registered_symbols="
              << book_directory_audit.registered_symbols
              << " materialized_books="
              << book_directory_audit.materialized_books
              << " prepared_books="
              << book_directory_audit.prepared_books
              << " registered_books_missing="
              << book_directory_audit.registered_books_missing
              << " unregistered_books_present="
              << book_directory_audit.unregistered_books_present
              << " descriptor_price_state_mismatches="
              << book_directory_audit.descriptor_price_state_mismatches
              << " descriptor_identity_mismatches="
              << book_directory_audit.descriptor_identity_mismatches
              << " committed_price_pages="
              << price_counters.committed_pages
              << " price_page_capacity="
              << price_counters.page_capacity
              << " price_page_capacity_failures="
              << price_counters.page_capacity_failures
              << " end_of_stream_accepted="
              << (end_of_stream_accepted ? "true" : "false")
              << "\n";

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

    if (channel.status != ChannelHealth::Good ||
        !channel.gap_buffer.empty() ||
        channel.phase != ChannelPhase::EndOfMessages ||
        final_live_orders != 0 || !book_directory_audit.clean() ||
        price_counters.page_capacity_failures != 0 ||
        !end_of_stream_accepted) {
      std::cerr << "channel did not finish a complete healthy session:"
                << " status=" << channelHealthName(channel.status)
                << " phase=" << channelPhaseName(channel.phase)
                << " final_live_orders=" << final_live_orders
                << " registered_symbols="
                << book_directory_audit.registered_symbols
                << " materialized_books="
                << book_directory_audit.materialized_books
                << " prepared_books="
                << book_directory_audit.prepared_books
                << " registered_books_missing="
                << book_directory_audit.registered_books_missing
                << " unregistered_books_present="
                << book_directory_audit.unregistered_books_present
                << " descriptor_price_state_mismatches="
                << book_directory_audit.descriptor_price_state_mismatches
                << " descriptor_identity_mismatches="
                << book_directory_audit.descriptor_identity_mismatches
                << " price_page_capacity_failures="
                << price_counters.page_capacity_failures
                << " end_of_stream_accepted="
                << (end_of_stream_accepted ? "true" : "false") << "\n";
      return EXIT_FAILURE;
    }

  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
