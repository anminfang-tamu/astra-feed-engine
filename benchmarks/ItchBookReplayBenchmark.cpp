#include "astra/book/BookManager.hpp"
#include "astra/core/Time.hpp"
#include "astra/parser/ItchParser.hpp"
#include "astra/symbol/StockDirectory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kUsage =
    "usage: astra_itch_book_replay_benchmark <ITCH-file> [--prefault] "
    "[--sample-every=N] [--warmup-book-messages=N] [--max-p50-ns=N] "
    "[--max-p99-ns=N] [--max-p99-9-ns=N] "
    "[--min-samples=N] [--expect-records=N] [--expect-bytes=N] "
    "[--capacity-profile-name=NAME] "
    "[--capacity-evidence-file=PATH] "
    "[--capacity-evidence-sha256=HEX] "
    "[--direct-order-slots=N] [--fallback-buckets=N] "
    "[--price-page-capacity=N] [--sample-capacity=N] "
    "[--start-gate-file=PATH] [--start-gate-timeout-ms=N] "
    "[--storage-plan-only] "
    "[--allow-legacy-eof-after-sc] "
    "[--require-zero-post-warmup-faults] "
    "[--mutation-digest] [--expect-mutation-digest=N] "
    "[--expect-semantic-mutation-digest=N]";

constexpr std::size_t kDefaultSampleCapacity = 8u * 1024u * 1024u;
constexpr std::array<unsigned char, 7> kBookMutationTypes{
    'A', 'F', 'E', 'C', 'X', 'D', 'U'};
constexpr std::uint64_t kSampleScheduleSeed = 0x61737472612d6974ull;
constexpr std::string_view kSampleScheduleId =
    "fixed_block_offset_v1_splitmix64_seed_61737472612d6974";
constexpr std::string_view kHotArenaSchema = "redesign_v1";
constexpr std::string_view kSemanticMutationDigestSchema =
    "applied_itch_book_semantics_v1_fnv1a64le";

struct Options {
  std::string path;
  std::uint64_t sample_every{64};
  std::uint64_t warmup_book_messages{1'000'000};
  std::uint64_t maximum_p50_ns{0};
  std::uint64_t maximum_p99_ns{0};
  std::uint64_t maximum_p999_ns{0};
  std::uint64_t minimum_samples{1'000};
  std::optional<std::uint64_t> expected_records;
  std::optional<std::uint64_t> expected_bytes;
  std::optional<std::uint64_t> expected_mutation_digest;
  std::optional<std::uint64_t> expected_semantic_mutation_digest;
  std::uint64_t direct_order_slots{0};
  std::uint32_t fallback_bucket_count{0};
  std::uint32_t price_page_capacity{0};
  std::optional<std::string> capacity_profile_name;
  std::optional<std::string> capacity_evidence_file;
  std::optional<std::string> capacity_evidence_sha256;
  std::size_t sample_capacity{kDefaultSampleCapacity};
  std::string start_gate_file;
  std::uint64_t start_gate_timeout_ms{600'000};
  bool prefault{false};
  bool mutation_digest{false};
  bool storage_plan_only{false};
  bool allow_legacy_eof_after_sc{false};
  bool require_zero_post_warmup_faults{false};
  bool direct_order_slots_explicit{false};
  bool fallback_bucket_count_explicit{false};
  bool price_page_capacity_explicit{false};
};

std::uint64_t parseU64(std::string_view value, const char *name) {
  std::uint64_t parsed = 0;
  const char *const begin = value.data();
  const char *const end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (value.empty() || result.ec != std::errc{} || result.ptr != end)
    throw std::invalid_argument(std::string("invalid ") + name);
  return parsed;
}

Options parseOptions(int argc, char **argv) {
  if (argc < 2)
    throw std::invalid_argument(std::string(kUsage));
  Options options;
  options.path = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    constexpr std::string_view sample_prefix = "--sample-every=";
    constexpr std::string_view warmup_prefix = "--warmup-book-messages=";
    constexpr std::string_view p50_prefix = "--max-p50-ns=";
    constexpr std::string_view p99_prefix = "--max-p99-ns=";
    constexpr std::string_view p999_prefix = "--max-p99-9-ns=";
    constexpr std::string_view minimum_samples_prefix = "--min-samples=";
    constexpr std::string_view records_prefix = "--expect-records=";
    constexpr std::string_view bytes_prefix = "--expect-bytes=";
    constexpr std::string_view capacity_profile_prefix =
        "--capacity-profile-name=";
    constexpr std::string_view capacity_evidence_file_prefix =
        "--capacity-evidence-file=";
    constexpr std::string_view capacity_evidence_sha256_prefix =
        "--capacity-evidence-sha256=";
    constexpr std::string_view direct_slots_prefix =
        "--direct-order-slots=";
    constexpr std::string_view fallback_buckets_prefix =
        "--fallback-buckets=";
    constexpr std::string_view price_pages_prefix =
        "--price-page-capacity=";
    constexpr std::string_view sample_capacity_prefix =
        "--sample-capacity=";
    constexpr std::string_view digest_prefix =
        "--expect-mutation-digest=";
    constexpr std::string_view semantic_digest_prefix =
        "--expect-semantic-mutation-digest=";
    constexpr std::string_view start_gate_prefix =
        "--start-gate-file=";
    constexpr std::string_view start_gate_timeout_prefix =
        "--start-gate-timeout-ms=";
    if (argument == "--prefault") {
      options.prefault = true;
    } else if (argument == "--mutation-digest") {
      options.mutation_digest = true;
    } else if (argument == "--storage-plan-only") {
      options.storage_plan_only = true;
    } else if (argument == "--allow-legacy-eof-after-sc") {
      options.allow_legacy_eof_after_sc = true;
    } else if (argument == "--require-zero-post-warmup-faults") {
      options.require_zero_post_warmup_faults = true;
    } else if (argument.starts_with(sample_prefix)) {
      options.sample_every =
          parseU64(argument.substr(sample_prefix.size()), "sample interval");
    } else if (argument.starts_with(warmup_prefix)) {
      options.warmup_book_messages =
          parseU64(argument.substr(warmup_prefix.size()), "warmup count");
    } else if (argument.starts_with(p50_prefix)) {
      options.maximum_p50_ns =
          parseU64(argument.substr(p50_prefix.size()), "p50 gate");
    } else if (argument.starts_with(p99_prefix)) {
      options.maximum_p99_ns =
          parseU64(argument.substr(p99_prefix.size()), "p99 gate");
    } else if (argument.starts_with(p999_prefix)) {
      options.maximum_p999_ns =
          parseU64(argument.substr(p999_prefix.size()), "p99.9 gate");
    } else if (argument.starts_with(minimum_samples_prefix)) {
      options.minimum_samples = parseU64(
          argument.substr(minimum_samples_prefix.size()), "minimum samples");
    } else if (argument.starts_with(records_prefix)) {
      options.expected_records = parseU64(
          argument.substr(records_prefix.size()), "expected record count");
    } else if (argument.starts_with(bytes_prefix)) {
      options.expected_bytes =
          parseU64(argument.substr(bytes_prefix.size()), "expected byte count");
    } else if (argument.starts_with(capacity_profile_prefix)) {
      if (options.capacity_profile_name.has_value())
        throw std::invalid_argument("duplicate capacity profile name");
      options.capacity_profile_name =
          std::string(argument.substr(capacity_profile_prefix.size()));
      if (options.capacity_profile_name->empty())
        throw std::invalid_argument("capacity profile name cannot be empty");
    } else if (argument.starts_with(capacity_evidence_file_prefix)) {
      if (options.capacity_evidence_file.has_value())
        throw std::invalid_argument("duplicate capacity evidence file");
      options.capacity_evidence_file =
          std::string(argument.substr(capacity_evidence_file_prefix.size()));
      if (options.capacity_evidence_file->empty())
        throw std::invalid_argument("capacity evidence file cannot be empty");
    } else if (argument.starts_with(capacity_evidence_sha256_prefix)) {
      if (options.capacity_evidence_sha256.has_value())
        throw std::invalid_argument("duplicate capacity evidence SHA-256");
      options.capacity_evidence_sha256 =
          std::string(argument.substr(capacity_evidence_sha256_prefix.size()));
      if (options.capacity_evidence_sha256->empty())
        throw std::invalid_argument(
            "capacity evidence SHA-256 cannot be empty");
    } else if (argument.starts_with(direct_slots_prefix)) {
      options.direct_order_slots = parseU64(
          argument.substr(direct_slots_prefix.size()), "direct order slots");
      options.direct_order_slots_explicit = true;
    } else if (argument.starts_with(fallback_buckets_prefix)) {
      const std::uint64_t buckets = parseU64(
          argument.substr(fallback_buckets_prefix.size()),
          "fallback bucket count");
      if (buckets > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("fallback bucket count exceeds uint32");
      options.fallback_bucket_count = static_cast<std::uint32_t>(buckets);
      options.fallback_bucket_count_explicit = true;
    } else if (argument.starts_with(price_pages_prefix)) {
      const std::uint64_t pages = parseU64(
          argument.substr(price_pages_prefix.size()), "price page capacity");
      if (pages >= std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "price page capacity must be below UINT32_MAX");
      options.price_page_capacity = static_cast<std::uint32_t>(pages);
      options.price_page_capacity_explicit = true;
    } else if (argument.starts_with(sample_capacity_prefix)) {
      const std::uint64_t capacity = parseU64(
          argument.substr(sample_capacity_prefix.size()), "sample capacity");
      if (capacity > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("sample capacity exceeds size_t");
      options.sample_capacity = static_cast<std::size_t>(capacity);
    } else if (argument.starts_with(digest_prefix)) {
      options.expected_mutation_digest = parseU64(
          argument.substr(digest_prefix.size()), "expected mutation digest");
      options.mutation_digest = true;
    } else if (argument.starts_with(semantic_digest_prefix)) {
      options.expected_semantic_mutation_digest = parseU64(
          argument.substr(semantic_digest_prefix.size()),
          "expected semantic mutation digest");
      options.mutation_digest = true;
    } else if (argument.starts_with(start_gate_prefix)) {
      options.start_gate_file =
          std::string(argument.substr(start_gate_prefix.size()));
      if (options.start_gate_file.empty())
        throw std::invalid_argument("start gate file cannot be empty");
    } else if (argument.starts_with(start_gate_timeout_prefix)) {
      options.start_gate_timeout_ms = parseU64(
          argument.substr(start_gate_timeout_prefix.size()),
          "start gate timeout");
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }
  if (options.sample_every == 0)
    throw std::invalid_argument("sample interval must be nonzero");
  if (options.minimum_samples == 0)
    throw std::invalid_argument("minimum samples must be nonzero");
  if (options.direct_order_slots_explicit && options.direct_order_slots == 0)
    throw std::invalid_argument("direct order slots must be nonzero");
  if (options.fallback_bucket_count_explicit &&
      (options.fallback_bucket_count == 0 ||
       (options.fallback_bucket_count &
        (options.fallback_bucket_count - 1)) != 0)) {
    throw std::invalid_argument(
        "fallback bucket count must be a nonzero power of two");
  }
  if (options.price_page_capacity_explicit &&
      options.price_page_capacity == 0) {
    throw std::invalid_argument("price page capacity must be nonzero");
  }
  if (options.sample_capacity == 0)
    throw std::invalid_argument("sample capacity must be nonzero");
  if (options.minimum_samples > options.sample_capacity)
    throw std::invalid_argument(
        "minimum samples cannot exceed sample capacity");
  if (options.start_gate_timeout_ms == 0 ||
      options.start_gate_timeout_ms > 3'600'000) {
    throw std::invalid_argument(
        "start gate timeout must be in [1, 3600000] milliseconds");
  }
  const unsigned int capacity_identity_fields =
      static_cast<unsigned int>(options.capacity_profile_name.has_value()) +
      static_cast<unsigned int>(options.capacity_evidence_file.has_value()) +
      static_cast<unsigned int>(options.capacity_evidence_sha256.has_value());
  if (capacity_identity_fields != 0 && capacity_identity_fields != 3) {
    throw std::invalid_argument(
        "capacity profile name, evidence file, and evidence SHA-256 "
        "must be supplied together");
  }
  const unsigned int explicit_capacity_fields =
      static_cast<unsigned int>(options.direct_order_slots_explicit) +
      static_cast<unsigned int>(options.fallback_bucket_count_explicit) +
      static_cast<unsigned int>(options.price_page_capacity_explicit);
  if (capacity_identity_fields == 0 && explicit_capacity_fields != 3) {
    throw std::invalid_argument(
        "supply checksum-bound capacity evidence or all of "
        "--direct-order-slots, --fallback-buckets, and "
        "--price-page-capacity");
  }
  const bool latency_gate_enabled = options.maximum_p50_ns != 0 ||
                                    options.maximum_p99_ns != 0 ||
                                    options.maximum_p999_ns != 0;
  if (options.mutation_digest && latency_gate_enabled) {
    throw std::invalid_argument(
        "mutation digest and latency threshold gates require separate runs");
  }
  return options;
}

struct ReplayCapacityProfile {
  bool bound{false};
  std::string evidence_schema;
  std::string name;
  std::string evidence_sha256;
  std::string corpus_manifest_sha256;
  std::string profiler_sha256;
  std::string profile_output_sha256;
  BookManagerConfig config;
  BookCapacityEvidence evidence;
  BookCapacityHeadroom minimum_headroom;
  BookCapacityHeadroom effective_headroom;
};

void requireCapacityMatch(std::string_view name, std::uint64_t supplied,
                          std::uint64_t manifested) {
  if (supplied != manifested) {
    throw std::invalid_argument(std::string(name) +
                                " differs from capacity evidence manifest");
  }
}

ReplayCapacityProfile resolveCapacityProfile(const Options &options) {
  if (options.capacity_profile_name.has_value()) {
    BookCapacityEvidenceManifest manifest =
        loadBookCapacityEvidenceManifest(
            *options.capacity_evidence_file, *options.capacity_profile_name,
            *options.capacity_evidence_sha256);
    if (options.direct_order_slots_explicit) {
      requireCapacityMatch("direct order slots", options.direct_order_slots,
                           manifest.profile.config.orders.direct_slots);
    }
    if (options.fallback_bucket_count_explicit) {
      requireCapacityMatch(
          "fallback bucket count", options.fallback_bucket_count,
          manifest.profile.config.orders.fallback_bucket_count);
    }
    if (options.price_page_capacity_explicit) {
      requireCapacityMatch("price page capacity",
                           options.price_page_capacity,
                           manifest.profile.config.prices.page_capacity);
    }
    manifest.profile.config.orders.prefault = options.prefault;
    manifest.profile.config.prices.prefault = options.prefault;
    const BookCapacityValidation validation =
        validateBookCapacityProfile(manifest.profile);
    std::string profile_output_sha256 =
        manifest.profile_output_sha256.empty()
            ? "not-recorded-v1"
            : std::move(manifest.profile_output_sha256);
    return {true,
            std::move(manifest.schema),
            std::move(manifest.profile.name),
            std::move(manifest.profile.evidence_sha256),
            std::move(manifest.corpus_manifest_sha256),
            std::move(manifest.profiler_sha256),
            std::move(profile_output_sha256),
            manifest.profile.config,
            manifest.profile.evidence,
            manifest.profile.minimum_headroom,
            validation.effective_headroom};
  }

  BookManagerConfig config{
      {options.direct_order_slots, options.fallback_bucket_count,
       options.prefault},
      {options.price_page_capacity, options.prefault}};
  // Small, explicit capacities remain useful for unit/development replay, but
  // live acceptance rejects this deliberately unbound identity.
  return {false, "unbound_development_v1", "unbound-development", "none",
          "none", "none", "none", config, {}, {}, {}};
}

void printCapacityProfile(std::ostream &output,
                          const ReplayCapacityProfile &profile) {
  output << " capacity_profile_bound=" << (profile.bound ? 1 : 0)
         << " capacity_evidence_schema=" << profile.evidence_schema
         << " capacity_profile_name=" << profile.name
         << " capacity_evidence_sha256=" << profile.evidence_sha256
         << " capacity_corpus_manifest_sha256="
         << profile.corpus_manifest_sha256
         << " capacity_profiler_sha256=" << profile.profiler_sha256
         << " capacity_profile_output_sha256="
         << profile.profile_output_sha256
         << " capacity_profiled_max_order_ref="
         << profile.evidence.profiled_max_order_reference
         << " capacity_profiled_unique_price_pages="
         << profile.evidence.profiled_unique_price_pages
         << " capacity_minimum_direct_order_headroom="
         << profile.minimum_headroom.direct_order_reference_slots
         << " capacity_effective_direct_order_headroom="
         << profile.effective_headroom.direct_order_reference_slots
         << " capacity_minimum_price_page_headroom="
         << profile.minimum_headroom.price_pages
         << " capacity_effective_price_page_headroom="
         << profile.effective_headroom.price_pages;
}

void requireStartGateAbsent(const std::string &path) {
  if (path.empty())
    return;
  if (::access(path.c_str(), F_OK) == 0)
    throw std::runtime_error("start gate file already exists");
  if (errno != ENOENT)
    throw std::runtime_error("cannot inspect start gate file");
}

void waitForStartGate(const std::string &path,
                      std::uint64_t timeout_ms) {
  if (path.empty())
    return;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (::access(path.c_str(), F_OK) == 0)
      return;
    if (errno != ENOENT)
      throw std::runtime_error("cannot inspect start gate file");
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("start gate wait timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

std::uint16_t readU16BE(const unsigned char *bytes) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
}

std::uint64_t readU64BE(const unsigned char *bytes) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index)
    value = (value << 8) | bytes[index];
  return value;
}

bool isBookMutation(unsigned char type) noexcept {
  switch (type) {
  case 'A':
  case 'F':
  case 'E':
  case 'C':
  case 'X':
  case 'D':
  case 'U':
    return true;
  default:
    return false;
  }
}

std::uint64_t deterministicMix(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

bool selectSample(std::uint64_t relative_book_message,
                  std::uint64_t sample_every) noexcept {
  if (sample_every == 1)
    return true;
  const std::uint64_t block = relative_book_message / sample_every;
  const std::uint64_t offset =
      deterministicMix(block ^ kSampleScheduleSeed) % sample_every;
  return relative_book_message % sample_every == offset;
}

constexpr std::uint64_t kDigestOffset = 14695981039346656037ull;
constexpr std::uint64_t kDigestPrime = 1099511628211ull;

void digestByte(std::uint64_t &digest, std::uint8_t value) noexcept {
  digest ^= value;
  digest *= kDigestPrime;
}

void digestU64(std::uint64_t &digest, std::uint64_t value) noexcept {
  for (unsigned shift = 0; shift < 64; shift += 8)
    digestByte(digest, static_cast<std::uint8_t>(value >> shift));
}

void digestBytes(std::uint64_t &digest,
                 std::span<const unsigned char> bytes) noexcept {
  digestU64(digest, bytes.size());
  for (const unsigned char byte : bytes)
    digestByte(digest, byte);
}

void digestString(std::uint64_t &digest, std::string_view text) noexcept {
  digestU64(digest, text.size());
  for (const char character : text)
    digestByte(digest, static_cast<std::uint8_t>(character));
}

std::uint32_t readU32BE(const unsigned char *bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

struct SemanticPreMutation {
  std::uint64_t order_reference{0};
  std::uint32_t raw_price{0};
  std::uint16_t locate{0};
  OrderSide side{static_cast<OrderSide>(0)};
  bool found{false};
};

// Correctness-only observer. Page handles are used solely as transient keys
// for recovering a live order's logical page index. They, lookup paths, and
// addresses are deliberately absent from the digest so different physical
// layouts can prove the same logical mutation sequence and post-state.
class SemanticMutationDigester final {
public:
  explicit SemanticMutationDigester(std::uint32_t price_page_capacity)
      : page_owner_by_handle_(
            static_cast<std::size_t>(price_page_capacity) + 1,
            kUnboundPageOwner) {
    digestString(digest_, kSemanticMutationDigestSchema);
  }

  SemanticPreMutation
  captureBefore(std::span<const unsigned char> message,
                const BookManager &books) const {
    SemanticPreMutation before{};
    if (message.empty() || message[0] == 'A' || message[0] == 'F')
      return before;
    if (message.size() < 19)
      return before;

    before.order_reference = readU64BE(message.data() + 11);
    const astra::book::OrderState *const state =
        books.orderTable().findState(before.order_reference);
    if (state == nullptr)
      return before;

    before.raw_price = logicalPrice(*state);
    before.locate = state->locate;
    before.side = decodeSide(state->side);
    before.found = true;
    return before;
  }

  void observeApplied(std::uint64_t applied_ordinal,
                      std::span<const unsigned char> message,
                      const SemanticPreMutation &before,
                      const BookManager &books) {
    if (message.size() < 3)
      throw std::runtime_error(
          "semantic digest: applied message lacks stock locate");
    const unsigned char type = message[0];
    const std::uint16_t locate = readU16BE(message.data() + 1);
    const OrderBook *const book = books.getOrderBook(locate);
    if (book == nullptr)
      throw std::runtime_error(
          "semantic digest: applied mutation has no prepared order book");

    std::uint64_t post_reference = 0;
    std::uint32_t affected_price = 0;
    OrderSide affected_side = static_cast<OrderSide>(0);
    if (type == 'A' || type == 'F') {
      if (message.size() < 36)
        throw std::runtime_error("semantic digest: truncated add order");
      post_reference = readU64BE(message.data() + 11);
      affected_price = readU32BE(message.data() + 32);
      affected_side = decodeSide(message[19]);
      bindPostOrder(post_reference, locate, affected_price, affected_side,
                    books);
    } else if (type == 'U') {
      if (message.size() < 35 || !before.found)
        throw std::runtime_error(
            "semantic digest: replace lacks logical pre-state");
      post_reference = readU64BE(message.data() + 19);
      affected_price = readU32BE(message.data() + 31);
      affected_side = before.side;
      bindPostOrder(post_reference, locate, affected_price, affected_side,
                    books);
    } else {
      if (!before.found)
        throw std::runtime_error(
            "semantic digest: existing-order mutation lacks pre-state");
      post_reference = before.order_reference;
      affected_price = before.raw_price;
      affected_side = before.side;
    }

    // Domain-separated, canonical little-endian integer encoding. The raw
    // ITCH message supplies the type and all logical wire fields; the applied
    // ordinal identifies its position among semantic mutations without
    // depending on unrelated records interspersed in the trace.
    digestByte(digest_, 0x01);
    digestU64(digest_, applied_ordinal);
    digestBytes(digest_, message);

    digestByte(digest_, 0x02);
    digestU64(digest_, locate);
    digestU64(digest_, book->liveOrderCount());
    const TopOfBook top = book->getTopOfBook();
    digestByte(digest_, top.has_bid ? 1 : 0);
    digestU64(digest_, top.bid_price);
    digestU64(digest_, top.bid_qty);
    digestByte(digest_, top.has_ask ? 1 : 0);
    digestU64(digest_, top.ask_price);
    digestU64(digest_, top.ask_qty);

    if (type == 'U') {
      digestOrderPostState(0x11, before.order_reference, books);
      digestOrderPostState(0x12, post_reference, books);
      digestLevelPostState(0x21, before.locate, before.raw_price,
                           before.side, books);
      digestLevelPostState(0x22, locate, affected_price, affected_side,
                           books);
    } else {
      digestOrderPostState(0x10, post_reference, books);
      digestLevelPostState(0x20, locate, affected_price, affected_side,
                           books);
    }
  }

  std::uint64_t value() const noexcept { return digest_; }

private:
  static constexpr std::uint64_t kUnboundPageOwner =
      std::numeric_limits<std::uint64_t>::max();

  static OrderSide decodeSide(std::uint8_t side) {
    if (side == static_cast<std::uint8_t>(OrderSide::Buy) || side == 'B')
      return OrderSide::Buy;
    if (side == static_cast<std::uint8_t>(OrderSide::Sell) || side == 'S')
      return OrderSide::Sell;
    throw std::runtime_error("semantic digest: invalid order side");
  }

  static std::uint64_t pageOwner(std::uint16_t locate,
                                 std::uint16_t page_index) noexcept {
    return (static_cast<std::uint64_t>(locate) << 16) | page_index;
  }

  std::uint32_t logicalPrice(const astra::book::OrderState &state) const {
    const std::size_t handle = state.price_page_handle;
    if (handle == astra::book::kInvalidPricePageHandle ||
        handle >= page_owner_by_handle_.size()) {
      throw std::runtime_error(
          "semantic digest: order has invalid price-page handle");
    }
    const std::uint64_t owner = page_owner_by_handle_[handle];
    if (owner == kUnboundPageOwner ||
        static_cast<std::uint16_t>(owner >> 16) != state.locate) {
      throw std::runtime_error(
          "semantic digest: price-page logical owner is unknown");
    }
    return astra::book::joinPrice(static_cast<std::uint16_t>(owner),
                                  state.price_level_index);
  }

  void bindPostOrder(std::uint64_t reference, std::uint16_t locate,
                     std::uint32_t raw_price, OrderSide expected_side,
                     const BookManager &books) {
    const astra::book::OrderState *const state =
        books.orderTable().findState(reference);
    if (state == nullptr || state->locate != locate ||
        decodeSide(state->side) != expected_side) {
      throw std::runtime_error(
          "semantic digest: applied add/replace has inconsistent order");
    }
    const astra::book::PriceParts parts = astra::book::splitPrice(raw_price);
    if (state->price_level_index != parts.level_index) {
      throw std::runtime_error(
          "semantic digest: order has inconsistent logical price level");
    }
    const std::size_t handle = state->price_page_handle;
    if (handle == astra::book::kInvalidPricePageHandle ||
        handle >= page_owner_by_handle_.size()) {
      throw std::runtime_error(
          "semantic digest: order has out-of-range price-page handle");
    }
    const std::uint64_t owner = pageOwner(locate, parts.page_index);
    std::uint64_t &known_owner = page_owner_by_handle_[handle];
    if (known_owner != kUnboundPageOwner && known_owner != owner) {
      throw std::runtime_error(
          "semantic digest: price-page handle changed logical owner");
    }
    known_owner = owner;
  }

  void digestOrderPostState(std::uint8_t role, std::uint64_t reference,
                            const BookManager &books) {
    digestByte(digest_, role);
    digestU64(digest_, reference);
    const astra::book::OrderState *const state =
        books.orderTable().findState(reference);
    digestByte(digest_, state == nullptr ? 0 : 1);
    if (state == nullptr)
      return;
    digestU64(digest_, state->qty);
    digestU64(digest_, logicalPrice(*state));
    digestU64(digest_, state->locate);
    digestByte(digest_, static_cast<std::uint8_t>(decodeSide(state->side)));
    digestByte(digest_, astra::book::isActive(*state) ? 1 : 0);
  }

  void digestLevelPostState(std::uint8_t role, std::uint16_t locate,
                            std::uint32_t raw_price, OrderSide side,
                            const BookManager &books) {
    digestByte(digest_, role);
    digestU64(digest_, locate);
    digestU64(digest_, raw_price);
    digestByte(digest_, static_cast<std::uint8_t>(side));
    const astra::book::PricePageReservation reservation =
        books.priceLevels().reservePrice(locate, raw_price);
    if (!reservation.valid() || reservation.requires_commit) {
      throw std::runtime_error(
          "semantic digest: affected logical price page is absent");
    }
    const astra::book::PriceLevelState *const level =
        books.priceLevels().resolveLevel(reservation.address, side);
    if (level == nullptr)
      throw std::runtime_error(
          "semantic digest: affected logical price level is absent");
    digestU64(digest_, level->total_qty);
    digestU64(digest_, level->order_count);
  }

  std::vector<std::uint64_t> page_owner_by_handle_;
  std::uint64_t digest_{kDigestOffset};
};

void observeAppliedMutation(std::uint64_t &digest, std::uint64_t record,
                            std::span<const unsigned char> message,
                            const BookManager &books) {
  if (message.size() < 3)
    throw std::runtime_error("applied book message lacks stock locate");
  const std::uint16_t locate = readU16BE(message.data() + 1);
  const OrderBook *book = books.getOrderBook(locate);
  if (book == nullptr)
    throw std::runtime_error("applied mutation has no prepared order book");

  // Hash the input and its post-mutation observable state. This executes only
  // after the ending timestamp, so correctness proof work cannot inflate the
  // latency samples.
  digestU64(digest, record);
  digestU64(digest, message.size());
  for (const unsigned char byte : message)
    digestByte(digest, byte);
  digestU64(digest, locate);
  digestU64(digest, book->liveOrderCount());
  const TopOfBook top = book->getTopOfBook();
  digestU64(digest, top.bid_price);
  digestU64(digest, top.bid_qty);
  digestU64(digest, top.ask_price);
  digestU64(digest, top.ask_qty);
  digestByte(digest, top.has_bid ? 1 : 0);
  digestByte(digest, top.has_ask ? 1 : 0);

  const std::size_t reference_offset = message[0] == 'U' ? 19u : 11u;
  if (message.size() < reference_offset + sizeof(std::uint64_t))
    throw std::runtime_error("applied book message lacks order reference");
  const std::uint64_t reference =
      readU64BE(message.data() + reference_offset);
  const astra::book::ConstOrderLookup lookup =
      books.orderTable().find(reference);
  digestU64(digest, reference);
  digestByte(digest, static_cast<std::uint8_t>(lookup.path));
  digestByte(digest, lookup.found() ? 1 : 0);
  if (lookup.found()) {
    const astra::book::OrderState &state = *lookup.state;
    digestU64(digest, state.qty);
    digestU64(digest, state.price_page_handle);
    digestU64(digest, state.price_level_index);
    digestU64(digest, state.locate);
    digestByte(digest, state.side);
    digestByte(digest, state.flags);

    const OrderSide side = static_cast<OrderSide>(state.side);
    const astra::book::PriceAddress address{state.price_page_handle,
                                            state.price_level_index, 0};
    const astra::book::PriceLevelState *level =
        books.priceLevels().resolveLevel(address, side);
    if (level == nullptr)
      throw std::runtime_error("live order has no resolvable price level");
    digestU64(digest, level->total_qty);
    digestU64(digest, level->order_count);
  }
}

std::uint64_t percentile(const std::vector<std::uint64_t> &values,
                         std::uint32_t numerator,
                         std::uint32_t denominator) {
  const std::size_t rank =
      (values.size() * static_cast<std::size_t>(numerator) + denominator - 1) /
      denominator;
  const std::size_t index = std::min(values.size() - 1, rank - 1);
  return values[index];
}

struct Distribution {
  std::uint64_t p50;
  std::uint64_t p90;
  std::uint64_t p99;
  std::uint64_t p999;
  std::uint64_t maximum;
};

struct TimedSample {
  std::uint64_t ticks;
  unsigned char type;
};

Distribution summarize(std::vector<std::uint64_t> &ticks) {
  if (ticks.empty())
    throw std::runtime_error("no timed book samples were collected");
  for (std::uint64_t &sample : ticks)
    sample = elapsedRdtscNs(0, sample);
  std::sort(ticks.begin(), ticks.end());
  return {percentile(ticks, 50, 100), percentile(ticks, 90, 100),
          percentile(ticks, 99, 100), percentile(ticks, 999, 1000),
          ticks.back()};
}

void printTypeDistribution(unsigned char type, std::size_t sample_count,
                           const Distribution &distribution) {
  std::cout << "itch_book_replay_type"
            << " type=" << static_cast<char>(type)
            << " sample_count=" << sample_count
            << " p50_ns=" << distribution.p50
            << " p90_ns=" << distribution.p90
            << " p99_ns=" << distribution.p99
            << " p99_9_ns=" << distribution.p999
            << " max_ns=" << distribution.maximum << '\n';
}

void printStorageArenaSizes(std::ostream &output,
                            const BookStorageFootprint &storage) {
  output << " order_direct_mapped_bytes="
         << storage.order_direct_mapped_bytes
         << " order_fallback_mapped_bytes="
         << storage.order_fallback_mapped_bytes
         << " book_descriptors_mapped_bytes="
         << storage.descriptor_mapped_bytes
         << " price_roots_mapped_bytes="
         << storage.price_roots_mapped_bytes
         << " price_prepared_books_mapped_bytes="
         << storage.price_prepared_books_mapped_bytes
         << " price_pages_mapped_bytes="
         << storage.price_pages_mapped_bytes
         << " price_page_owners_mapped_bytes="
         << storage.price_page_owners_mapped_bytes
         << " price_page_summaries_mapped_bytes="
         << storage.price_page_summaries_mapped_bytes
         << " price_page_occupancy_mapped_bytes="
         << storage.price_page_occupancy_mapped_bytes
         << " price_book_summaries_mapped_bytes="
         << storage.price_book_summaries_mapped_bytes
         << " price_book_occupancy_mapped_bytes="
         << storage.price_book_occupancy_mapped_bytes;
}

void printArenaRange(std::ostream &output, std::string_view identifier,
                     const void *base, std::size_t mapped_bytes) {
  output << ' ' << identifier << "_base="
         << reinterpret_cast<std::uintptr_t>(base) << ' ' << identifier
         << "_mapped_bytes=" << mapped_bytes;
}

void printReadyArenaRanges(std::ostream &output, const BookManager &books) {
  const auto &orders = books.orderTable();
  const auto &prices = books.priceLevels();
  printArenaRange(output, "order_direct", orders.directArenaBase(),
                  orders.directArenaMappedBytes());
  printArenaRange(output, "order_fallback", orders.fallbackArenaBase(),
                  orders.fallbackArenaMappedBytes());
  printArenaRange(output, "book_descriptors",
                  books.bookDescriptorsArenaBase(),
                  books.bookDescriptorsArenaMappedBytes());
  printArenaRange(output, "price_roots", prices.rootsArenaBase(),
                  prices.rootsArenaMappedBytes());
  printArenaRange(output, "price_prepared_books",
                  prices.preparedBooksArenaBase(),
                  prices.preparedBooksArenaMappedBytes());
  printArenaRange(output, "price_pages", prices.pageArenaBase(),
                  prices.pageArenaMappedBytes());
  printArenaRange(output, "price_page_owners", prices.pageOwnersArenaBase(),
                  prices.pageOwnersArenaMappedBytes());
  printArenaRange(output, "price_page_summaries",
                  prices.pageSummariesArenaBase(),
                  prices.pageSummariesArenaMappedBytes());
  printArenaRange(output, "price_page_occupancy",
                  prices.pageOccupancyArenaBase(),
                  prices.pageOccupancyArenaMappedBytes());
  printArenaRange(output, "price_book_summaries",
                  prices.bookSummariesArenaBase(),
                  prices.bookSummariesArenaMappedBytes());
  printArenaRange(output, "price_book_occupancy",
                  prices.bookOccupancyArenaBase(),
                  prices.bookOccupancyArenaMappedBytes());
}

void printEffectiveArenaSizes(std::ostream &output,
                              const BookStorageFootprint &storage) {
  output << " effective_order_direct_mapped_bytes="
         << storage.order_direct_mapped_bytes
         << " effective_order_fallback_mapped_bytes="
         << storage.order_fallback_mapped_bytes
         << " effective_book_descriptors_mapped_bytes="
         << storage.descriptor_mapped_bytes
         << " effective_price_roots_mapped_bytes="
         << storage.price_roots_mapped_bytes
         << " effective_price_prepared_books_mapped_bytes="
         << storage.price_prepared_books_mapped_bytes
         << " effective_price_pages_mapped_bytes="
         << storage.price_pages_mapped_bytes
         << " effective_price_page_owners_mapped_bytes="
         << storage.price_page_owners_mapped_bytes
         << " effective_price_page_summaries_mapped_bytes="
         << storage.price_page_summaries_mapped_bytes
         << " effective_price_page_occupancy_mapped_bytes="
         << storage.price_page_occupancy_mapped_bytes
         << " effective_price_book_summaries_mapped_bytes="
         << storage.price_book_summaries_mapped_bytes
         << " effective_price_book_occupancy_mapped_bytes="
         << storage.price_book_occupancy_mapped_bytes;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << kUsage << '\n'
                << "Use --mutation-digest for a correctness replay. Omit it "
                   "for latency runs so state observation cannot warm the "
                   "next mutation's cache lines. Sampling selects one "
                   "fixed-seed pseudo-random position per --sample-every "
                   "block. The default --min-samples is 1000. A correctness "
                   "replay emits both the same-binary physical mutation "
                   "digest and a versioned, layout-independent semantic "
                   "mutation digest. Capacity has no built-in default: supply "
                   "the checksum-bound profile/evidence/SHA-256 trio, or all "
                   "three explicit capacity flags for an unbound development "
                   "replay.\n";
      return EXIT_SUCCESS;
    }
    const Options options = parseOptions(argc, argv);
    const ReplayCapacityProfile capacity_profile =
        resolveCapacityProfile(options);
    const BookManagerConfig config = capacity_profile.config;
    const BookStorageFootprint planned_storage =
        estimateBookStorageFootprint(config);
    std::cout << "itch_book_replay_storage_plan"
              << " hot_arena_schema=" << kHotArenaSchema;
    printCapacityProfile(std::cout, capacity_profile);
    std::cout
              << " system_page_bytes=" << planned_storage.system_page_bytes
              << " mapped_array_bytes=" << planned_storage.mapped_array_bytes
              << " direct_orders_mapped_bytes="
              << planned_storage.order_direct_mapped_bytes;
    printStorageArenaSizes(std::cout, planned_storage);
    std::cout << " descriptor_bytes=" << planned_storage.descriptor_slot_bytes
              << " planned_storage_bytes="
              << planned_storage.planned_storage_bytes
              << " direct_order_slots=" << config.orders.direct_slots
              << " fallback_buckets=" << config.orders.fallback_bucket_count
              << " price_page_capacity=" << config.prices.page_capacity
              << " prefault=" << (config.prices.prefault ? 1 : 0) << '\n'
              << std::flush;
    if (options.storage_plan_only)
      return EXIT_SUCCESS;

    std::ifstream input(options.path, std::ios::binary);
    if (!input.is_open())
      throw std::runtime_error("failed to open input file");
    std::vector<char> file_buffer(16u * 1024u * 1024u);
    input.rdbuf()->pubsetbuf(file_buffer.data(),
                            static_cast<std::streamsize>(file_buffer.size()));

    astra::symbol::StockDirectory symbols;
    BookManager books(config);
    const BookManagerConfig &effective_config = books.config();
    const BookStorageFootprint &effective_storage =
        books.storageFootprint();
    ItchParser parser(symbols, books);
    const TimeCalibration &calibration = timeCalibration();

    std::vector<unsigned char> message(1u << 16);
    std::vector<TimedSample> samples;
    // Keep collection allocation- and demand-fault-free during replay. resize
    // writes every backing page before the feed; clear retains that resident
    // capacity without adding elements to the measured sample set.
    samples.resize(options.sample_capacity);
    samples.clear();
    std::optional<SemanticMutationDigester> semantic_digester;
    if (options.mutation_digest)
      semantic_digester.emplace(effective_config.prices.page_capacity);
    requireStartGateAbsent(options.start_gate_file);
    std::uint64_t records = 0;
    std::uint64_t bytes = 0;
    std::uint64_t book_messages = 0;
    std::uint64_t applied_book_mutations = 0;
    std::uint64_t mutation_digest = kDigestOffset;
    std::uint64_t prelude_records = 0;
    std::uint64_t prelude_bytes = 0;
    rusage post_warmup_start{};
    rusage post_warmup_end{};
    bool post_warmup_usage_started = false;
    bool ready_emitted = false;
    bool last_message_was_end_of_messages = false;
    const char *binaryfile_completion = "none";

    for (;;) {
      unsigned char length_bytes[2]{};
      input.read(reinterpret_cast<char *>(length_bytes), 2);
      if (input.gcount() == 0 && input.eof()) {
        if (!options.allow_legacy_eof_after_sc ||
            !last_message_was_end_of_messages) {
          throw std::runtime_error(
              "physical EOF before zero-length BinaryFILE terminator");
        }
        binaryfile_completion = "legacy_sc_eof";
        break;
      }
      if (input.gcount() != 2)
        throw std::runtime_error("truncated record length");
      const std::uint16_t length = readU16BE(length_bytes);
      if (length == 0) {
        bytes += 2;
        if (input.peek() != std::char_traits<char>::eof()) {
          throw std::runtime_error(
              "data follows zero-length BinaryFILE terminator");
        }
        binaryfile_completion = "terminator";
        break;
      }
      input.read(reinterpret_cast<char *>(message.data()), length);
      if (input.gcount() != static_cast<std::streamsize>(length))
        throw std::runtime_error("truncated ITCH record");
      ++records;
      bytes += static_cast<std::uint64_t>(length) + 2;
      last_message_was_end_of_messages =
          length == 12 && message[0] == 'S' && message[11] == 'C';

      const bool book_message = isBookMutation(message[0]);
      if (!ready_emitted && book_message) {
        throw std::runtime_error(
            "book mutation arrived before System Event S readiness gate");
      }
      const bool after_warmup =
          book_message && book_messages >= options.warmup_book_messages;
      const bool sample =
          after_warmup &&
          selectSample(book_messages - options.warmup_book_messages,
                       options.sample_every);
      SemanticPreMutation semantic_before{};
      if (book_message && semantic_digester.has_value()) {
        semantic_before = semantic_digester->captureBefore(
            std::span<const unsigned char>(message.data(), length), books);
      }
      std::uint64_t start = 0;
      if (sample)
        start = rdtsc();
      const bool applied = parser.handleMessage(std::span<const std::byte>(
          reinterpret_cast<const std::byte *>(message.data()), length));
      if (sample) {
        const std::uint64_t elapsed_ticks = rdtsc() - start;
        if (samples.size() == samples.capacity()) {
          throw std::runtime_error(
              "timed sample capacity exceeded; increase --sample-every");
        }
        samples.push_back(TimedSample{elapsed_ticks, message[0]});
      }
      if (!parser.lastError().empty()) {
        std::cerr << "record=" << records
                  << " type=" << static_cast<char>(message[0])
                  << " error=\"" << parser.lastError() << "\"\n";
        return EXIT_FAILURE;
      }
      if (!ready_emitted &&
          parser.channelPhase() == ChannelPhase::SystemHours) {
        prelude_records = records;
        prelude_bytes = bytes;
        std::cout << "itch_book_replay_ready"
                  << " hot_arena_schema=" << kHotArenaSchema;
        printCapacityProfile(std::cout, capacity_profile);
        std::cout
                  << " prefault="
                  << (effective_config.prices.prefault ? 1 : 0)
                  << " effective_storage_bytes="
                  << effective_storage.planned_storage_bytes
                  << " direct_orders_base="
                  << reinterpret_cast<std::uintptr_t>(
                         books.orderTable().directArenaBase())
                  << " direct_orders_mapped_bytes="
                  << books.orderTable().directArenaMappedBytes();
        printReadyArenaRanges(std::cout, books);
        std::cout << " sample_capacity=" << options.sample_capacity
                  << " sample_storage_prefaulted=1"
                  << " prelude_records=" << prelude_records
                  << " prelude_bytes=" << prelude_bytes
                  << " sample_schedule_id=" << kSampleScheduleId
                  << " sample_every=" << options.sample_every
                  << " warmup_book_messages="
                  << options.warmup_book_messages
                  << " start_gate_enabled="
                  << (options.start_gate_file.empty() ? 0 : 1) << '\n'
                  << std::flush;
        waitForStartGate(options.start_gate_file,
                         options.start_gate_timeout_ms);
        ready_emitted = true;
        if (options.warmup_book_messages == 0) {
          if (::getrusage(RUSAGE_SELF, &post_warmup_start) != 0)
            throw std::runtime_error("getrusage failed before replay");
          post_warmup_usage_started = true;
        }
      }
      if (book_message)
        ++book_messages;
      if (!post_warmup_usage_started && ready_emitted &&
          book_messages == options.warmup_book_messages) {
        if (::getrusage(RUSAGE_SELF, &post_warmup_start) != 0)
          throw std::runtime_error("getrusage failed after warmup");
        post_warmup_usage_started = true;
      }
      if (book_message && applied) {
        ++applied_book_mutations;
        if (options.mutation_digest) {
          observeAppliedMutation(
              mutation_digest, records,
              std::span<const unsigned char>(message.data(), length), books);
          semantic_digester->observeApplied(
              applied_book_mutations,
              std::span<const unsigned char>(message.data(), length),
              semantic_before, books);
        }
      }
    }

    if (!ready_emitted) {
      throw std::runtime_error(
          "trace ended before System Event S prepared the book universe");
    }

    if (post_warmup_usage_started) {
      if (::getrusage(RUSAGE_SELF, &post_warmup_end) != 0)
        throw std::runtime_error("getrusage failed after replay");
    } else {
      post_warmup_start = {};
      post_warmup_end = {};
    }
    const std::uint64_t post_warmup_minor_faults =
        static_cast<std::uint64_t>(post_warmup_end.ru_minflt -
                                   post_warmup_start.ru_minflt);
    const std::uint64_t post_warmup_major_faults =
        static_cast<std::uint64_t>(post_warmup_end.ru_majflt -
                                   post_warmup_start.ru_majflt);

    if (options.expected_records.has_value() &&
        records != *options.expected_records) {
      std::cerr << "record-count gate failed: " << records << " != "
                << *options.expected_records << '\n';
      return EXIT_FAILURE;
    }
    if (options.expected_bytes.has_value() && bytes != *options.expected_bytes) {
      std::cerr << "byte-count gate failed: " << bytes << " != "
                << *options.expected_bytes << '\n';
      return EXIT_FAILURE;
    }
    if (samples.size() < options.minimum_samples) {
      std::cerr << "minimum-sample gate failed: " << samples.size() << " < "
                << options.minimum_samples << '\n';
      return EXIT_FAILURE;
    }

    std::uint64_t final_live_orders = 0;
    std::uint64_t state_checksum = 0;
    for (std::size_t slot = 1; slot < BookManager::kMaxStockLocate; ++slot) {
      const auto locate = static_cast<std::uint16_t>(slot);
      const OrderBook *book = books.getOrderBook(locate);
      if (book == nullptr)
        continue;
      final_live_orders += book->liveOrderCount();
      const TopOfBook top = book->getTopOfBook();
      state_checksum ^= top.bid_price + (top.bid_qty << 1) +
                        (top.ask_price << 2) + (top.ask_qty << 3) + slot;
    }

    std::vector<std::uint64_t> scratch;
    scratch.reserve(samples.size());
    for (const TimedSample &sample : samples)
      scratch.push_back(sample.ticks);
    const Distribution distribution = summarize(scratch);

    std::array<std::optional<Distribution>, kBookMutationTypes.size()>
        type_distributions{};
    std::array<std::size_t, kBookMutationTypes.size()> type_sample_counts{};
    for (std::size_t type_index = 0;
         type_index < kBookMutationTypes.size(); ++type_index) {
      scratch.clear();
      for (const TimedSample &sample : samples) {
        if (sample.type == kBookMutationTypes[type_index])
          scratch.push_back(sample.ticks);
      }
      type_sample_counts[type_index] = scratch.size();
      if (!scratch.empty())
        type_distributions[type_index] = summarize(scratch);
    }

    const auto price_counters = books.priceLevels().counters();
    std::cout << "itch_book_replay"
              << " hot_arena_schema=" << kHotArenaSchema;
    printCapacityProfile(std::cout, capacity_profile);
    std::cout
              << " path=" << std::quoted(options.path)
              << " records=" << records << " bytes=" << bytes
              << " binaryfile_completion=" << binaryfile_completion
              << " book_messages=" << book_messages
              << " applied_book_mutations=" << applied_book_mutations
              << " sample_count=" << samples.size()
              << " sample_every=" << options.sample_every
              << " sample_strategy=fixed_seed_block_offset"
              << " sample_schedule_id=" << kSampleScheduleId
              << " warmup_book_messages=" << options.warmup_book_messages
              << " min_samples=" << options.minimum_samples
              << " sample_capacity=" << options.sample_capacity
              << " sample_storage_prefaulted=1"
              << " prelude_records=" << prelude_records
              << " prelude_bytes=" << prelude_bytes
              << " post_warmup_minor_faults="
              << post_warmup_minor_faults
              << " post_warmup_major_faults="
              << post_warmup_major_faults
              << " prefault=" << (effective_config.prices.prefault ? 1 : 0)
              << " direct_order_slots="
              << effective_config.orders.direct_slots
              << " fallback_buckets="
              << effective_config.orders.fallback_bucket_count
              << " price_page_capacity="
              << effective_config.prices.page_capacity
              << " storage_system_page_bytes="
              << effective_storage.system_page_bytes
              << " effective_mapped_bytes="
              << effective_storage.mapped_array_bytes
              << " effective_direct_orders_mapped_bytes="
              << effective_storage.order_direct_mapped_bytes;
    printEffectiveArenaSizes(std::cout, effective_storage);
    std::cout << " effective_descriptor_bytes="
              << effective_storage.descriptor_slot_bytes
              << " effective_storage_bytes="
              << effective_storage.planned_storage_bytes
              << " price_pages=" << price_counters.committed_pages
              << " price_capacity_failures="
              << price_counters.page_capacity_failures
              << " rdtsc_overhead_ticks="
              << calibration.rdtsc_overhead_ticks
              << " rdtsc_ticks_per_second="
              << calibration.rdtsc_ticks_per_second
              << " now_ns_overhead_ns=" << calibration.now_ns_overhead_ns
              << " final_live_orders=" << final_live_orders
              << " phase=" << static_cast<int>(parser.channelPhase())
              << " state_checksum=" << state_checksum
              << " mutation_digest_enabled="
              << (options.mutation_digest ? 1 : 0);
    if (options.mutation_digest) {
      std::cout << " mutation_digest=" << mutation_digest
                << " semantic_mutation_digest_enabled=1"
                << " semantic_mutation_digest_schema="
                << kSemanticMutationDigestSchema
                << " semantic_mutation_digest="
                << semantic_digester->value();
    } else {
      std::cout << " semantic_mutation_digest_enabled=0";
    }
    std::cout
              << " p50_ns=" << distribution.p50
              << " p90_ns=" << distribution.p90
              << " p99_ns=" << distribution.p99
              << " p99_9_ns=" << distribution.p999
              << " max_ns=" << distribution.maximum << '\n';

    for (std::size_t type_index = 0;
         type_index < kBookMutationTypes.size(); ++type_index) {
      if (type_distributions[type_index].has_value()) {
        printTypeDistribution(kBookMutationTypes[type_index],
                              type_sample_counts[type_index],
                              *type_distributions[type_index]);
      } else {
        std::cout << "itch_book_replay_type"
                  << " type=" << static_cast<char>(kBookMutationTypes[type_index])
                  << " sample_count=0 distribution=unavailable\n";
      }
    }

    if (final_live_orders != 0 ||
        price_counters.page_capacity_failures != 0 ||
        parser.channelPhase() != ChannelPhase::EndOfMessages) {
      std::cerr << "full-trace correctness gate failed\n";
      return EXIT_FAILURE;
    }
    if (options.expected_mutation_digest.has_value() &&
        mutation_digest != *options.expected_mutation_digest) {
      std::cerr << "mutation digest gate failed: " << mutation_digest << " != "
                << *options.expected_mutation_digest << '\n';
      return EXIT_FAILURE;
    }
    if (options.expected_semantic_mutation_digest.has_value() &&
        semantic_digester->value() !=
            *options.expected_semantic_mutation_digest) {
      std::cerr << "semantic mutation digest gate failed: "
                << semantic_digester->value() << " != "
                << *options.expected_semantic_mutation_digest << '\n';
      return EXIT_FAILURE;
    }
    if (options.require_zero_post_warmup_faults &&
        (post_warmup_minor_faults != 0 ||
         post_warmup_major_faults != 0)) {
      std::cerr << "post-warmup page-fault gate failed: minor="
                << post_warmup_minor_faults
                << " major=" << post_warmup_major_faults << '\n';
      return EXIT_FAILURE;
    }
#if defined(__x86_64__) || defined(__i386__)
    if (options.maximum_p50_ns != 0 &&
        distribution.p50 > options.maximum_p50_ns) {
      std::cerr << "p50 gate failed: " << distribution.p50 << " > "
                << options.maximum_p50_ns << " ns\n";
      return EXIT_FAILURE;
    }
    if (options.maximum_p99_ns != 0 &&
        distribution.p99 > options.maximum_p99_ns) {
      std::cerr << "p99 gate failed: " << distribution.p99 << " > "
                << options.maximum_p99_ns << " ns\n";
      return EXIT_FAILURE;
    }
    if (options.maximum_p999_ns != 0 &&
        distribution.p999 > options.maximum_p999_ns) {
      std::cerr << "p99.9 gate failed: " << distribution.p999 << " > "
                << options.maximum_p999_ns << " ns\n";
      return EXIT_FAILURE;
    }
#else
    if (options.maximum_p50_ns != 0 || options.maximum_p99_ns != 0 ||
        options.maximum_p999_ns != 0) {
      std::cerr << "latency gate failed: RDTSCP is unavailable on this "
                   "host\n";
      return EXIT_FAILURE;
    }
#endif
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "ITCH book replay benchmark failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}
