#include "astra/book/PriceLevelStore.hpp"
#include "astra/protocol/ItchMessageLength.hpp"
#include "astra/protocol/ItchPrice.hpp"
#include "astra/symbol/StockLocate.hpp"
#include "astra/utils/Sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

constexpr std::size_t kTickerLength = 8;
constexpr std::size_t kWireLocateCount = 1u << 16;
constexpr std::uint64_t kNanosecondsPerDay = 86'400'000'000'000ULL;
constexpr std::array<unsigned char, 6> kDailySystemEvents{
    'O', 'S', 'Q', 'M', 'E', 'C'};

std::filesystem::path resolveExecutablePath(std::string_view argv0) {
#if defined(__linux__)
  (void)argv0;
  return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
  (void)argv0;
  std::vector<char> path_buffer(1024);
  std::uint32_t required_size =
      static_cast<std::uint32_t>(path_buffer.size());
  if (_NSGetExecutablePath(path_buffer.data(), &required_size) != 0) {
    path_buffer.resize(required_size);
    if (_NSGetExecutablePath(path_buffer.data(), &required_size) != 0)
      throw std::runtime_error("cannot resolve running profiler executable");
  }
  return std::filesystem::canonical(path_buffer.data());
#else
  if (argv0.empty())
    throw std::runtime_error("profiler executable path is empty");

  const std::filesystem::path supplied(argv0);
  if (!supplied.has_parent_path()) {
    throw std::runtime_error(
        "profiler argv[0] must contain a path on this platform");
  }
  return std::filesystem::canonical(supplied);
#endif
}

std::string sha256File(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open())
    throw std::runtime_error("cannot open profiler executable for SHA-256");
  astra::utils::Sha256 digest;
  std::array<unsigned char, 1024u * 1024u> buffer{};
  std::uint64_t bytes = 0;
  while (input.read(reinterpret_cast<char *>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size())) ||
         input.gcount() != 0) {
    const std::size_t count = static_cast<std::size_t>(input.gcount());
    digest.update(
        std::span<const unsigned char>(buffer.data(), count));
    bytes += count;
  }
  if (!input.eof())
    throw std::runtime_error("cannot read complete profiler executable");
  if (bytes == 0)
    throw std::runtime_error("profiler executable is empty");
  return digest.finalizeHex();
}

uint16_t readU16BE(const unsigned char *bytes) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                               static_cast<uint16_t>(bytes[1]));
}

uint32_t readU32BE(const unsigned char *bytes) noexcept {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

uint64_t readU48BE(const unsigned char *bytes) noexcept {
  uint64_t value = 0;
  for (unsigned i = 0; i < 6; ++i) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

uint64_t readU64BE(const unsigned char *bytes) noexcept {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

struct DirectoryIdentity {
  std::array<unsigned char, kTickerLength> ticker{};
  bool seen{false};
};

struct OrderState {
  uint32_t price{0};
  uint32_t qty{0};
  uint16_t locate{0};
  unsigned char side{0};
};

struct LevelState {
  uint64_t total_qty{0};
  uint32_t order_count{0};
};

// Exact, sparse lifetime set for the 64-bit order-reference domain. Each
// allocated page covers 65,536 references with one bit per reference, so a
// sequential full day costs one bit per observed ID while sparse high IDs do
// not force a giant dense allocation.
struct SeenOrderReferencePage {
  static constexpr std::size_t kWordCount = (1u << 16) / 64;
  std::array<std::uint64_t, kWordCount> words{};
};

struct Stats {
  std::array<uint64_t, 256> type_counts{};
  std::array<DirectoryIdentity, kWireLocateCount> directory{};
  std::unordered_set<uint32_t> price_pages;
  std::unordered_map<uint64_t, OrderState> live_orders;
  std::unordered_map<uint64_t, SeenOrderReferencePage>
      seen_order_reference_pages;
  std::unordered_map<uint64_t, LevelState> active_levels;
  std::unordered_map<uint32_t, uint32_t> active_page_level_counts;
  std::unordered_set<uint32_t> ever_active_pages;
  std::unordered_map<uint64_t, uint16_t> directory_ticker_locates;
  std::array<uint32_t, kWireLocateCount> live_orders_by_locate{};
  std::array<uint32_t, kWireLocateCount> live_order_hwm_by_locate{};
  std::array<uint32_t, kWireLocateCount> levels_by_locate{};
  std::array<uint32_t, kWireLocateCount> level_hwm_by_locate{};
  std::array<uint32_t, kWireLocateCount> pages_by_locate{};
  std::array<uint32_t, kWireLocateCount> page_hwm_by_locate{};
  std::array<uint32_t, kWireLocateCount> first_order_price{};
  std::array<uint32_t, kWireLocateCount> min_order_price{};
  std::array<uint32_t, kWireLocateCount> max_order_price{};
  std::array<bool, kWireLocateCount> has_order_price{};
  uint64_t records{0};
  uint64_t bytes{0};
  uint64_t malformed_records{0};
  uint64_t zero_length_records{0};
  const char *binaryfile_completion{"none"};
  uint64_t order_refs_above_u32{0};
  uint64_t prices_above_documented_max{0};
  uint64_t directory_before_system_hours{0};
  uint64_t directory_after_system_hours{0};
  uint64_t new_directory_after_system_hours{0};
  uint64_t repeated_directory_after_system_hours{0};
  uint64_t conflicting_directory_after_system_hours{0};
  uint64_t conflicting_directory_identities{0};
  uint64_t invalid_directory_identities{0};
  uint64_t duplicate_directory_tickers{0};
  uint64_t invalid_timestamps{0};
  uint64_t order_adds_before_directory{0};
  uint64_t order_add_stock_mismatches{0};
  uint64_t book_messages_before_system_hours{0};
  uint64_t system_event_e{0};
  uint64_t daily_system_events_seen{0};
  uint64_t system_event_sequence_errors{0};
  uint64_t unsupported_system_event_codes{0};
  uint64_t messages_after_system_event_e{0};
  uint64_t cancels_after_system_event_e{0};
  uint64_t deletes_after_system_event_e{0};
  uint64_t broken_trades_after_system_event_e{0};
  uint64_t invalid_messages_after_system_event_e{0};
  uint64_t max_order_ref{0};
  uint64_t max_match_number{0};
  uint64_t new_order_refs{0};
  uint64_t new_order_refs_above_u32{0};
  uint64_t min_new_order_ref{std::numeric_limits<uint64_t>::max()};
  uint64_t max_new_order_ref{0};
  uint64_t previous_new_order_ref{0};
  uint64_t new_order_ref_transitions{0};
  uint64_t monotonic_new_order_ref_transitions{0};
  uint64_t max_forward_order_ref_gap{0};
  uint64_t duplicate_order_refs{0};
  uint64_t missing_deletes{0};
  uint64_t missing_cancels{0};
  uint64_t missing_executions{0};
  uint64_t missing_replaces{0};
  uint64_t same_reference_replaces{0};
  uint64_t over_cancels{0};
  uint64_t over_executions{0};
  uint64_t zero_quantity_mutations{0};
  uint64_t invalid_order_sides{0};
  uint64_t locate_mismatches{0};
  uint64_t reconstruction_invariant_failures{0};
  uint64_t live_order_hwm{0};
  uint64_t order_prices{0};
  uint64_t symbols_with_order_prices{0};
  uint64_t prices_outside_first_window{0};
  uint64_t active_level_count{0};
  uint64_t active_level_hwm{0};
  uint64_t active_page_count{0};
  uint64_t active_page_hwm{0};
  uint64_t max_level_qty{0};
  uint32_t min_order_book_price{std::numeric_limits<uint32_t>::max()};
  uint32_t max_order_book_price{0};
  uint32_t min_price{std::numeric_limits<uint32_t>::max()};
  uint32_t max_price{0};
  uint16_t max_stock_locate{0};
  uint16_t max_message_length{0};
  bool system_hours_started{false};
  bool system_hours_ended{false};
};

void observeOrderRef(Stats &stats, uint64_t order_ref) noexcept {
  if (order_ref > stats.max_order_ref) {
    stats.max_order_ref = order_ref;
  }
  if (order_ref > std::numeric_limits<uint32_t>::max()) {
    ++stats.order_refs_above_u32;
  }
}

void observeMatch(Stats &stats, uint64_t match_number) noexcept {
  if (match_number > stats.max_match_number) {
    stats.max_match_number = match_number;
  }
}

void observePrice(Stats &stats, uint16_t locate, uint32_t price) {
  if (price < stats.min_price) {
    stats.min_price = price;
  }
  if (price > stats.max_price) {
    stats.max_price = price;
  }
  // Current Nasdaq TotalView-ITCH 5.0 documents Price(4) up to
  // 200,000.0000, or 2,000,000,000 in raw units.
  if (price > astra::protocol::kMaxItchPrice4) {
    ++stats.prices_above_documented_max;
  }
  const uint32_t locate_and_page =
      (static_cast<uint32_t>(locate) << 16) | (price >> 16);
  stats.price_pages.insert(locate_and_page);
}

constexpr uint64_t levelKey(uint16_t locate, unsigned char side,
                            uint32_t price) noexcept {
  return (static_cast<uint64_t>(locate) << 33) |
         (static_cast<uint64_t>(side == 'S') << 32) | price;
}

constexpr uint32_t pageKey(uint16_t locate, uint32_t price) noexcept {
  return (static_cast<uint32_t>(locate) << 16) | (price >> 16);
}

void observeNewOrderRef(Stats &stats, uint64_t order_ref) noexcept {
  if (stats.new_order_refs != 0) {
    ++stats.new_order_ref_transitions;
    if (order_ref > stats.previous_new_order_ref) {
      ++stats.monotonic_new_order_ref_transitions;
      const uint64_t gap = order_ref - stats.previous_new_order_ref;
      if (gap > stats.max_forward_order_ref_gap) {
        stats.max_forward_order_ref_gap = gap;
      }
    }
  }

  ++stats.new_order_refs;
  stats.previous_new_order_ref = order_ref;
  if (order_ref < stats.min_new_order_ref) {
    stats.min_new_order_ref = order_ref;
  }
  if (order_ref > stats.max_new_order_ref) {
    stats.max_new_order_ref = order_ref;
  }
  if (order_ref > std::numeric_limits<uint32_t>::max()) {
    ++stats.new_order_refs_above_u32;
  }
}

bool claimDayUniqueOrderRef(Stats &stats, uint64_t order_ref) {
  const std::uint64_t page_index = order_ref >> 16;
  const std::uint16_t bit_index =
      static_cast<std::uint16_t>(order_ref);
  auto [page_it, inserted] =
      stats.seen_order_reference_pages.try_emplace(page_index);
  (void)inserted;
  std::uint64_t &word =
      page_it->second.words[bit_index >> 6];
  const std::uint64_t mask =
      std::uint64_t{1} << (bit_index & 63u);
  if ((word & mask) != 0) {
    ++stats.duplicate_order_refs;
    return false;
  }
  word |= mask;
  return true;
}

void observeOrderPrice(Stats &stats, uint16_t locate, uint32_t price) noexcept {
  ++stats.order_prices;
  if (price < stats.min_order_book_price) {
    stats.min_order_book_price = price;
  }
  if (price > stats.max_order_book_price) {
    stats.max_order_book_price = price;
  }

  if (!stats.has_order_price[locate]) {
    stats.has_order_price[locate] = true;
    stats.first_order_price[locate] = price;
    stats.min_order_price[locate] = price;
    stats.max_order_price[locate] = price;
    ++stats.symbols_with_order_prices;
    return;
  }

  if (price < stats.min_order_price[locate]) {
    stats.min_order_price[locate] = price;
  }
  if (price > stats.max_order_price[locate]) {
    stats.max_order_price[locate] = price;
  }

  // The former centered table admitted 32,768 raw ticks below its reference,
  // but only 32,767 above it (indices 0 through 65,535, center 32,768).
  const uint32_t reference = stats.first_order_price[locate];
  const bool outside = price >= reference
                           ? static_cast<uint64_t>(price - reference) >= 32'768
                           : static_cast<uint64_t>(reference - price) > 32'768;
  stats.prices_outside_first_window += outside;
}

bool addToLevel(Stats &stats, const OrderState &order) {
  const uint64_t key = levelKey(order.locate, order.side, order.price);
  auto [level_it, created_level] =
      stats.active_levels.try_emplace(key, LevelState{});

  if (created_level) {
    ++stats.active_level_count;
    stats.active_level_hwm =
        std::max(stats.active_level_hwm, stats.active_level_count);

    uint32_t &locate_levels = stats.levels_by_locate[order.locate];
    ++locate_levels;
    stats.level_hwm_by_locate[order.locate] =
        std::max(stats.level_hwm_by_locate[order.locate], locate_levels);

    const uint32_t page_key = pageKey(order.locate, order.price);
    auto [page_it, created_page] =
        stats.active_page_level_counts.try_emplace(page_key, 0);
    if (created_page) {
      stats.ever_active_pages.insert(page_key);
      ++stats.active_page_count;
      stats.active_page_hwm =
          std::max(stats.active_page_hwm, stats.active_page_count);

      uint32_t &locate_pages = stats.pages_by_locate[order.locate];
      ++locate_pages;
      stats.page_hwm_by_locate[order.locate] =
          std::max(stats.page_hwm_by_locate[order.locate], locate_pages);
    }
    ++page_it->second;
  }

  LevelState &level = level_it->second;
  level.total_qty += order.qty;
  ++level.order_count;
  stats.max_level_qty = std::max(stats.max_level_qty, level.total_qty);
  return true;
}

bool removeFromLevel(Stats &stats, const OrderState &order,
                     uint32_t removed_qty, bool remove_order) {
  const uint64_t key = levelKey(order.locate, order.side, order.price);
  const auto level_it = stats.active_levels.find(key);
  if (level_it == stats.active_levels.end() ||
      level_it->second.total_qty < removed_qty ||
      level_it->second.order_count == 0) {
    ++stats.reconstruction_invariant_failures;
    return false;
  }

  LevelState &level = level_it->second;
  level.total_qty -= removed_qty;
  if (!remove_order) {
    if (level.total_qty == 0) {
      ++stats.reconstruction_invariant_failures;
      return false;
    }
    return true;
  }

  --level.order_count;
  if (level.order_count != 0) {
    if (level.total_qty == 0) {
      ++stats.reconstruction_invariant_failures;
      return false;
    }
    return true;
  }
  if (level.total_qty != 0) {
    ++stats.reconstruction_invariant_failures;
    return false;
  }

  stats.active_levels.erase(level_it);
  --stats.active_level_count;
  --stats.levels_by_locate[order.locate];

  const uint32_t page_key = pageKey(order.locate, order.price);
  const auto page_it = stats.active_page_level_counts.find(page_key);
  if (page_it == stats.active_page_level_counts.end() || page_it->second == 0) {
    ++stats.reconstruction_invariant_failures;
    return false;
  }
  --page_it->second;
  if (page_it->second == 0) {
    stats.active_page_level_counts.erase(page_it);
    --stats.active_page_count;
    --stats.pages_by_locate[order.locate];
  }
  return true;
}

bool addLiveOrder(Stats &stats, uint64_t order_ref,
                  const OrderState &order) {
  const auto [order_it, inserted] = stats.live_orders.emplace(order_ref, order);
  (void)order_it;
  if (!inserted) {
    ++stats.duplicate_order_refs;
    return false;
  }
  if (!addToLevel(stats, order)) {
    stats.live_orders.erase(order_ref);
    return false;
  }

  const uint64_t live_count = stats.live_orders.size();
  stats.live_order_hwm = std::max(stats.live_order_hwm, live_count);
  uint32_t &locate_live = stats.live_orders_by_locate[order.locate];
  ++locate_live;
  stats.live_order_hwm_by_locate[order.locate] =
      std::max(stats.live_order_hwm_by_locate[order.locate], locate_live);
  return true;
}

bool removeLiveOrder(Stats &stats,
                     std::unordered_map<uint64_t, OrderState>::iterator it) {
  const OrderState order = it->second;
  if (!removeFromLevel(stats, order, order.qty, true)) {
    return false;
  }
  --stats.live_orders_by_locate[order.locate];
  stats.live_orders.erase(it);
  return true;
}

void checkLocate(Stats &stats, uint16_t wire_locate,
                 const OrderState &order) noexcept {
  stats.locate_mismatches += wire_locate != order.locate;
}

void profileAdd(Stats &stats, uint16_t locate, uint64_t order_ref,
                uint32_t price, uint32_t qty, unsigned char side) {
  observeNewOrderRef(stats, order_ref);
  observeOrderPrice(stats, locate, price);
  if (!claimDayUniqueOrderRef(stats, order_ref))
    return;
  if (qty == 0) {
    ++stats.zero_quantity_mutations;
    return;
  }
  if (side != 'B' && side != 'S') {
    ++stats.invalid_order_sides;
    return;
  }
  (void)addLiveOrder(stats, order_ref,
                     OrderState{price, qty, locate, side});
}

void profileCancel(Stats &stats, uint16_t locate, uint64_t order_ref,
                   uint32_t canceled_qty) {
  const auto order_it = stats.live_orders.find(order_ref);
  if (order_it == stats.live_orders.end()) {
    ++stats.missing_cancels;
    return;
  }
  checkLocate(stats, locate, order_it->second);
  if (canceled_qty == 0) {
    ++stats.zero_quantity_mutations;
    return;
  }
  // ITCH X is a partial cancel. Equality and over-cancel are feed errors; D is
  // the message that removes an entire remaining order.
  if (canceled_qty >= order_it->second.qty) {
    ++stats.over_cancels;
    return;
  }
  if (!removeFromLevel(stats, order_it->second, canceled_qty, false)) {
    return;
  }
  order_it->second.qty -= canceled_qty;
}

void profileDelete(Stats &stats, uint16_t locate, uint64_t order_ref) {
  const auto order_it = stats.live_orders.find(order_ref);
  if (order_it == stats.live_orders.end()) {
    ++stats.missing_deletes;
    return;
  }
  checkLocate(stats, locate, order_it->second);
  (void)removeLiveOrder(stats, order_it);
}

void profileExecution(Stats &stats, uint16_t locate, uint64_t order_ref,
                      uint32_t executed_qty) {
  const auto order_it = stats.live_orders.find(order_ref);
  if (order_it == stats.live_orders.end()) {
    ++stats.missing_executions;
    return;
  }
  checkLocate(stats, locate, order_it->second);
  if (executed_qty == 0) {
    ++stats.zero_quantity_mutations;
    return;
  }
  if (executed_qty > order_it->second.qty) {
    ++stats.over_executions;
    return;
  }
  if (executed_qty == order_it->second.qty) {
    (void)removeLiveOrder(stats, order_it);
    return;
  }
  if (!removeFromLevel(stats, order_it->second, executed_qty, false)) {
    return;
  }
  order_it->second.qty -= executed_qty;
}

void profileReplace(Stats &stats, uint16_t locate, uint64_t old_order_ref,
                    uint64_t new_order_ref, uint32_t new_price,
                    uint32_t new_qty) {
  observeNewOrderRef(stats, new_order_ref);
  observeOrderPrice(stats, locate, new_price);

  if (old_order_ref == new_order_ref) {
    ++stats.same_reference_replaces;
    return;
  }
  if (!claimDayUniqueOrderRef(stats, new_order_ref))
    return;

  const auto old_it = stats.live_orders.find(old_order_ref);
  if (old_it == stats.live_orders.end()) {
    ++stats.missing_replaces;
    return;
  }
  checkLocate(stats, locate, old_it->second);
  if (new_qty == 0) {
    ++stats.zero_quantity_mutations;
    return;
  }
  if (stats.live_orders.find(new_order_ref) != stats.live_orders.end()) {
    ++stats.duplicate_order_refs;
    return;
  }

  const unsigned char side = old_it->second.side;
  const uint16_t order_locate = old_it->second.locate;
  if (!removeLiveOrder(stats, old_it)) {
    return;
  }
  (void)addLiveOrder(stats, new_order_ref,
                     OrderState{new_price, new_qty, order_locate, side});
}

bool tickerMatches(const DirectoryIdentity &identity,
                   const unsigned char *ticker) noexcept {
  return std::memcmp(identity.ticker.data(), ticker, kTickerLength) == 0;
}

bool tickerHasIdentity(const unsigned char *ticker) noexcept {
  for (std::size_t index = 0; index < kTickerLength; ++index) {
    if (ticker[index] != ' ' && ticker[index] != '\0')
      return true;
  }
  return false;
}

uint64_t tickerKey(const unsigned char *ticker) noexcept {
  return readU64BE(ticker);
}

void observeDirectory(Stats &stats, uint16_t locate,
                      const unsigned char *message) {
  if (locate == 0 || !tickerHasIdentity(message + 11)) {
    ++stats.invalid_directory_identities;
    return;
  }

  DirectoryIdentity &identity = stats.directory[locate];
  const bool existed = identity.seen;
  const bool same_identity = existed && tickerMatches(identity, message + 11);
  if (existed && !same_identity)
    ++stats.conflicting_directory_identities;

  if (stats.system_hours_started) {
    ++stats.directory_after_system_hours;
    if (!existed) {
      ++stats.new_directory_after_system_hours;
    } else if (same_identity) {
      ++stats.repeated_directory_after_system_hours;
    } else {
      ++stats.conflicting_directory_after_system_hours;
    }
  } else {
    ++stats.directory_before_system_hours;
  }

  if (!existed) {
    const uint64_t ticker_key = tickerKey(message + 11);
    const auto [ticker_it, inserted] =
        stats.directory_ticker_locates.emplace(ticker_key, locate);
    if (!inserted && ticker_it->second != locate) {
      ++stats.duplicate_directory_tickers;
    }
    std::memcpy(identity.ticker.data(), message + 11, kTickerLength);
    identity.seen = true;
  }
}

void observeAddIdentity(Stats &stats, uint16_t locate,
                        const unsigned char *message) noexcept {
  const DirectoryIdentity &identity = stats.directory[locate];
  if (!identity.seen) {
    ++stats.order_adds_before_directory;
  } else if (!tickerMatches(identity, message + 24)) {
    ++stats.order_add_stock_mismatches;
  }
}

void observeSystemEvent(Stats &stats, unsigned char event_code) noexcept {
  switch (event_code) {
  case 'O':
  case 'S':
  case 'Q':
  case 'M':
  case 'E':
  case 'C':
    if (stats.daily_system_events_seen >= kDailySystemEvents.size() ||
        event_code !=
            kDailySystemEvents[stats.daily_system_events_seen]) {
      ++stats.system_event_sequence_errors;
    } else {
      ++stats.daily_system_events_seen;
    }
    break;
  case 'A': // Emergency Market Condition - Halt
  case 'R': // Emergency Market Condition - Quote Only
  case 'B': // Emergency Market Condition - Resumption
    break;
  default:
    ++stats.unsupported_system_event_codes;
    break;
  }

  if (event_code == 'S') {
    stats.system_hours_started = true;
  } else if (event_code == 'E') {
    ++stats.system_event_e;
    stats.system_hours_ended = true;
  } else if (event_code == 'C') {
    stats.system_hours_ended = false;
  }
}

void observeMessage(Stats &stats, const unsigned char *message,
                    uint16_t length) {
  if (length == 0) {
    ++stats.zero_length_records;
    return;
  }

  const unsigned char type = message[0];
  ++stats.type_counts[type];
  if (length > stats.max_message_length) {
    stats.max_message_length = length;
  }
  const std::uint8_t expected_length =
      astra::protocol::expectedItchMessageLength(type);
  if (expected_length == 0 || length != expected_length) {
    ++stats.malformed_records;
    return;
  }

  const uint16_t locate = length >= 3 ? readU16BE(message + 1) : 0;
  if (readU48BE(message + 5) >= kNanosecondsPerDay) {
    ++stats.invalid_timestamps;
  }
  if (locate > stats.max_stock_locate) {
    stats.max_stock_locate = locate;
  }
  if (stats.system_hours_ended) {
    ++stats.messages_after_system_event_e;
    // The official 2026-06-12 archive carries valid partial X cancels in the
    // teardown tail even though the specification calls out only B and D.
    stats.cancels_after_system_event_e += type == 'X';
    stats.deletes_after_system_event_e += type == 'D';
    stats.broken_trades_after_system_event_e += type == 'B';
    stats.invalid_messages_after_system_event_e +=
        type != 'X' && type != 'D' && type != 'B' &&
        !(type == 'S' && message[11] == 'C');
  }
  if (!stats.system_hours_started &&
      (type == 'A' || type == 'F' || type == 'E' || type == 'C' ||
       type == 'X' || type == 'D' || type == 'U')) {
    ++stats.book_messages_before_system_hours;
  }

  switch (type) {
  case 'A':
  case 'F':
    if (length < (type == 'F' ? 40 : 36)) {
      ++stats.malformed_records;
      return;
    }
    {
      const uint64_t order_ref = readU64BE(message + 11);
      const uint32_t qty = readU32BE(message + 20);
      const uint32_t price = readU32BE(message + 32);
      observeOrderRef(stats, order_ref);
      observePrice(stats, locate, price);
      observeAddIdentity(stats, locate, message);
      profileAdd(stats, locate, order_ref, price, qty, message[19]);
    }
    return;
  case 'E':
    if (length < 31) {
      ++stats.malformed_records;
      return;
    }
    {
      const uint64_t order_ref = readU64BE(message + 11);
      observeOrderRef(stats, order_ref);
      profileExecution(stats, locate, order_ref, readU32BE(message + 19));
    }
    observeMatch(stats, readU64BE(message + 23));
    return;
  case 'C':
    if (length < 36) {
      ++stats.malformed_records;
      return;
    }
    {
      const uint64_t order_ref = readU64BE(message + 11);
      observeOrderRef(stats, order_ref);
      profileExecution(stats, locate, order_ref, readU32BE(message + 19));
    }
    observeMatch(stats, readU64BE(message + 23));
    observePrice(stats, locate, readU32BE(message + 32));
    return;
  case 'X':
    if (length < 23) {
      ++stats.malformed_records;
      return;
    }
    {
      const uint64_t order_ref = readU64BE(message + 11);
      observeOrderRef(stats, order_ref);
      profileCancel(stats, locate, order_ref, readU32BE(message + 19));
    }
    return;
  case 'D':
    if (length < 19) {
      ++stats.malformed_records;
      return;
    }
    {
      const uint64_t order_ref = readU64BE(message + 11);
      observeOrderRef(stats, order_ref);
      profileDelete(stats, locate, order_ref);
    }
    return;
  case 'U':
    if (length < 35) {
      ++stats.malformed_records;
      return;
    }
    {
      const uint64_t old_order_ref = readU64BE(message + 11);
      const uint64_t new_order_ref = readU64BE(message + 19);
      const uint32_t new_price = readU32BE(message + 31);
      observeOrderRef(stats, old_order_ref);
      observeOrderRef(stats, new_order_ref);
      observePrice(stats, locate, new_price);
      profileReplace(stats, locate, old_order_ref, new_order_ref, new_price,
                     readU32BE(message + 27));
    }
    return;
  case 'B':
    if (length < 19) {
      ++stats.malformed_records;
      return;
    }
    observeMatch(stats, readU64BE(message + 11));
    return;
  case 'R':
    if (length < 39) {
      ++stats.malformed_records;
      return;
    }
    observeDirectory(stats, locate, message);
    return;
  case 'S':
    if (length < 12) {
      ++stats.malformed_records;
      return;
    }
    observeSystemEvent(stats, message[11]);
    return;
  default:
    return;
  }
}

void printTopHwm(
    std::string_view label,
    const std::array<uint32_t, kWireLocateCount> &high_watermarks) {
  std::vector<std::pair<uint32_t, uint32_t>> ranked;
  ranked.reserve(kWireLocateCount);
  for (uint32_t locate = 0; locate < kWireLocateCount; ++locate) {
    if (high_watermarks[locate] != 0) {
      ranked.emplace_back(high_watermarks[locate], locate);
    }
  }
  std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first > rhs.first;
    }
    return lhs.second < rhs.second;
  });

  std::cout << label << '=';
  const std::size_t count = std::min<std::size_t>(20, ranked.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    std::cout << ranked[i].second << ':' << ranked[i].first;
  }
  std::cout << '\n';
}

void printTopPriceSpans(const Stats &stats) {
  std::vector<std::pair<uint32_t, uint32_t>> ranked;
  ranked.reserve(kWireLocateCount);
  for (uint32_t locate = 0; locate < kWireLocateCount; ++locate) {
    if (!stats.has_order_price[locate]) {
      continue;
    }
    ranked.emplace_back(stats.max_order_price[locate] -
                            stats.min_order_price[locate],
                        locate);
  }
  std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first > rhs.first;
    }
    return lhs.second < rhs.second;
  });

  std::cout << "top_price_span=";
  const std::size_t count = std::min<std::size_t>(20, ranked.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    std::cout << ranked[i].second << ':' << ranked[i].first;
  }
  std::cout << '\n';
}

uint64_t registeredDirectoryCount(const Stats &stats) noexcept {
  uint64_t count = 0;
  for (const DirectoryIdentity &identity : stats.directory) {
    count += identity.seen;
  }
  return count;
}

void printReconstructionStats(const Stats &stats) {
  const uint64_t min_order_ref =
      stats.new_order_refs == 0 ? 0 : stats.min_new_order_ref;
  const double monotonic_fraction =
      stats.new_order_ref_transitions == 0
          ? 0.0
          : static_cast<double>(stats.monotonic_new_order_ref_transitions) /
                static_cast<double>(stats.new_order_ref_transitions);
  const uint32_t min_order_price =
      stats.order_prices == 0 ? 0 : stats.min_order_book_price;

  std::cout << "orders"
            << " new_refs=" << stats.new_order_refs
            << " id_min=" << min_order_ref
            << " id_max=" << stats.max_new_order_ref
            << " id_gt_u32=" << stats.new_order_refs_above_u32
            << " monotonic_fraction=" << std::setprecision(6)
            << monotonic_fraction
            << " max_forward_gap=" << stats.max_forward_order_ref_gap
            << " seen_ref_pages="
            << stats.seen_order_reference_pages.size()
            << " live_final=" << stats.live_orders.size()
            << " live_hwm=" << stats.live_order_hwm
            << " duplicates=" << stats.duplicate_order_refs
            << " missing_D=" << stats.missing_deletes
            << " missing_X=" << stats.missing_cancels
            << " missing_EC=" << stats.missing_executions
            << " missing_U=" << stats.missing_replaces
            << " same_ref_U=" << stats.same_reference_replaces
            << " over_X=" << stats.over_cancels
            << " over_EC=" << stats.over_executions
            << " zero_qty=" << stats.zero_quantity_mutations
            << " invalid_sides=" << stats.invalid_order_sides
            << " locate_mismatches=" << stats.locate_mismatches
            << " invariant_failures="
            << stats.reconstruction_invariant_failures << '\n';

  std::cout << "prices"
            << " adds=" << stats.order_prices
            << " symbols=" << stats.symbols_with_order_prices
            << " min=" << min_order_price
            << " max=" << stats.max_order_book_price
            << " outside_first_65536_window="
            << stats.prices_outside_first_window
            << " active_levels_final=" << stats.active_level_count
            << " active_levels_hwm=" << stats.active_level_hwm
            << " active_price_pages_final=" << stats.active_page_count
            << " active_price_pages_hwm=" << stats.active_page_hwm
            << " ever_price_pages=" << stats.ever_active_pages.size()
            << " max_level_qty=" << stats.max_level_qty << '\n';

  const uint64_t registered = registeredDirectoryCount(stats);
  const uint64_t page_capacity = stats.ever_active_pages.size();
  const uint64_t page_metadata_slots = page_capacity + 1u;
  const uint64_t active_page_hwm_bytes =
      stats.active_page_hwm * sizeof(astra::book::PricePage);
  const uint64_t page_arena_bytes =
      page_capacity * sizeof(astra::book::PricePage);
  const uint64_t page_owner_bytes =
      page_metadata_slots *
      astra::book::PriceLevelStore::kPageOwnerStorageBytes;
  const uint64_t page_summary_bytes =
      page_metadata_slots *
      astra::book::PriceLevelStore::kPageSummaryStorageBytes;
  const uint64_t page_occupancy_bytes =
      page_metadata_slots * astra::book::PriceLevelStore::kSideCount *
      astra::book::PriceLevelStore::kBitmapStorageBytes;
  const uint64_t root_handle_bytes =
      astra::book::kPriceRootSlotCount *
      sizeof(astra::book::PricePageHandle);
  const uint64_t prepared_book_bytes =
      astra::symbol::kStockLocateSlots * sizeof(std::uint8_t);
  const uint64_t book_summary_bytes =
      astra::symbol::kStockLocateSlots *
      astra::book::PriceLevelStore::kBookSummaryStorageBytes;
  const uint64_t book_occupancy_bytes =
      astra::symbol::kStockLocateSlots *
      astra::book::PriceLevelStore::kSideCount *
      astra::book::PriceLevelStore::kBitmapStorageBytes;
  const uint64_t total_logical_bytes =
      page_arena_bytes + page_owner_bytes + page_summary_bytes +
      page_occupancy_bytes + root_handle_bytes + prepared_book_bytes +
      book_summary_bytes + book_occupancy_bytes;
  std::cout << "price_storage_logical_model"
            << " basis=observed_pages_no_headroom"
            << " registered_books=" << registered
            << " page_capacity=" << page_capacity
            << " page_bytes=" << sizeof(astra::book::PricePage)
            << " active_page_hwm_bytes=" << active_page_hwm_bytes
            << " page_arena_bytes=" << page_arena_bytes
            << " page_owner_bytes=" << page_owner_bytes
            << " page_summary_bytes=" << page_summary_bytes
            << " page_occupancy_bytes=" << page_occupancy_bytes
            << " fixed_root_handle_bytes=" << root_handle_bytes
            << " fixed_prepared_book_bytes=" << prepared_book_bytes
            << " fixed_book_summary_bytes=" << book_summary_bytes
            << " fixed_book_occupancy_bytes=" << book_occupancy_bytes
            << " total_logical_bytes=" << total_logical_bytes
            << " authoritative_plan=md_engine_book_storage_plan"
            << '\n';

  printTopHwm("top_live_hwm", stats.live_order_hwm_by_locate);
  printTopHwm("top_levels_hwm", stats.level_hwm_by_locate);
  printTopHwm("top_price_pages_hwm", stats.page_hwm_by_locate);
  printTopPriceSpans(stats);
}

void printStats(const Stats &stats, std::string_view path,
                std::string_view binaryfile_sha256,
                std::string_view profiler_sha256) {
  const uint32_t min_price =
      stats.min_price == std::numeric_limits<uint32_t>::max()
          ? 0
          : stats.min_price;
  std::cout << "itch_trace_profile"
            << " path=" << std::quoted(path) << " records=" << stats.records
            << " bytes=" << stats.bytes
            << " malformed_records=" << stats.malformed_records
            << " zero_length_records=" << stats.zero_length_records
            << " binaryfile_completion=" << stats.binaryfile_completion
            << " max_message_length=" << stats.max_message_length
            << " max_stock_locate=" << stats.max_stock_locate
            << " max_order_ref=" << stats.max_order_ref
            << " order_refs_above_u32=" << stats.order_refs_above_u32
            << " max_match_number=" << stats.max_match_number
            << " min_price=" << min_price << " max_price=" << stats.max_price
            << " prices_above_documented_max="
            << stats.prices_above_documented_max
            << " distinct_locate_price_pages=" << stats.price_pages.size()
            << " binaryfile_sha256=" << binaryfile_sha256
            << " profiler_sha256=" << profiler_sha256
            << '\n';

  std::cout << "lifecycle"
            << " directory_before_system_hours="
            << stats.directory_before_system_hours
            << " directory_after_system_hours="
            << stats.directory_after_system_hours
            << " new_directory_after_system_hours="
            << stats.new_directory_after_system_hours
            << " repeated_directory_after_system_hours="
            << stats.repeated_directory_after_system_hours
            << " conflicting_directory_after_system_hours="
            << stats.conflicting_directory_after_system_hours
            << " conflicting_directory_identities="
            << stats.conflicting_directory_identities
            << " invalid_directory_identities="
            << stats.invalid_directory_identities
            << " duplicate_directory_tickers="
            << stats.duplicate_directory_tickers
            << " invalid_timestamps=" << stats.invalid_timestamps
            << " order_adds_before_directory="
            << stats.order_adds_before_directory
            << " order_add_stock_mismatches="
            << stats.order_add_stock_mismatches
            << " book_messages_before_system_hours="
            << stats.book_messages_before_system_hours
            << " system_event_e=" << stats.system_event_e
            << " daily_system_events_seen="
            << stats.daily_system_events_seen
            << " system_event_sequence_errors="
            << stats.system_event_sequence_errors
            << " unsupported_system_event_codes="
            << stats.unsupported_system_event_codes
            << " messages_after_system_event_e="
            << stats.messages_after_system_event_e
            << " cancels_after_system_event_e="
            << stats.cancels_after_system_event_e
            << " deletes_after_system_event_e="
            << stats.deletes_after_system_event_e
            << " broken_trades_after_system_event_e="
            << stats.broken_trades_after_system_event_e
            << " invalid_messages_after_system_event_e="
            << stats.invalid_messages_after_system_event_e << '\n';

  std::cout << "message_counts";
  for (std::size_t type = 0; type < stats.type_counts.size(); ++type) {
    if (stats.type_counts[type] != 0) {
      std::cout << ' ' << static_cast<char>(type) << '='
                << stats.type_counts[type];
    }
  }
  std::cout << '\n';

  printReconstructionStats(stats);
}

bool semanticProfileClean(const Stats &stats) noexcept {
  return stats.prices_above_documented_max == 0 &&
         stats.conflicting_directory_after_system_hours == 0 &&
         stats.conflicting_directory_identities == 0 &&
         stats.invalid_directory_identities == 0 &&
         stats.duplicate_directory_tickers == 0 &&
         stats.invalid_timestamps == 0 &&
         stats.order_adds_before_directory == 0 &&
         stats.order_add_stock_mismatches == 0 &&
         stats.book_messages_before_system_hours == 0 &&
         stats.daily_system_events_seen == kDailySystemEvents.size() &&
         stats.system_event_sequence_errors == 0 &&
         stats.unsupported_system_event_codes == 0 &&
         stats.system_event_e == 1 &&
         stats.invalid_messages_after_system_event_e == 0 &&
         stats.duplicate_order_refs == 0 &&
         stats.missing_deletes == 0 &&
         stats.missing_cancels == 0 &&
         stats.missing_executions == 0 &&
         stats.missing_replaces == 0 &&
         stats.same_reference_replaces == 0 &&
         stats.over_cancels == 0 &&
         stats.over_executions == 0 &&
         stats.zero_quantity_mutations == 0 &&
         stats.invalid_order_sides == 0 &&
         stats.locate_mismatches == 0 &&
         stats.reconstruction_invariant_failures == 0 &&
         stats.live_orders.empty() &&
         stats.active_level_count == 0 &&
         stats.active_page_count == 0;
}

} // namespace

int main(int argc, char **argv) {
  const bool allow_legacy_eof_after_sc =
      argc == 3 &&
      std::string_view(argv[2]) == "--allow-legacy-eof-after-sc";
  if (argc < 2 || argc > 3 || (argc == 3 && !allow_legacy_eof_after_sc)) {
    std::cerr << "Usage: " << argv[0]
              << " <NASDAQ_ITCH50_file> [--allow-legacy-eof-after-sc]\n";
    return 2;
  }

  std::string profiler_sha256;
  try {
    profiler_sha256 = sha256File(resolveExecutablePath(argv[0]));
  } catch (const std::exception &error) {
    std::cerr << "failed to hash profiler executable: " << error.what()
              << '\n';
    return 2;
  }

  std::ifstream input(argv[1], std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "failed to open " << argv[1] << '\n';
    return 2;
  }

  std::vector<char> file_buffer(16 * 1024 * 1024);
  input.rdbuf()->pubsetbuf(file_buffer.data(),
                          static_cast<std::streamsize>(file_buffer.size()));
  std::vector<unsigned char> message(1u << 16);
  Stats stats;
  stats.price_pages.reserve(1u << 20);
  stats.live_orders.max_load_factor(0.70f);
  stats.live_orders.reserve(4u << 20);
  stats.seen_order_reference_pages.reserve(1u << 15);
  stats.active_levels.max_load_factor(0.70f);
  stats.active_levels.reserve(1u << 20);
  stats.active_page_level_counts.reserve(1u << 17);
  stats.ever_active_pages.reserve(1u << 17);
  stats.directory_ticker_locates.reserve(1u << 15);
  bool saw_message = false;
  bool saw_end_of_messages = false;
  astra::utils::Sha256 binaryfile_digest;

  for (;;) {
    unsigned char length_bytes[2]{};
    input.read(reinterpret_cast<char *>(length_bytes), 2);
    if (input.gcount() > 0) {
      binaryfile_digest.update(std::span<const unsigned char>(
          length_bytes, static_cast<std::size_t>(input.gcount())));
    }
    if (input.gcount() == 0 && input.eof()) {
      if (allow_legacy_eof_after_sc && saw_end_of_messages) {
        stats.binaryfile_completion = "legacy_sc_eof";
      } else {
        ++stats.malformed_records;
        stats.binaryfile_completion = "incomplete_eof";
        std::cerr
            << "physical EOF before zero-length BinaryFILE terminator\n";
      }
      break;
    }
    if (input.gcount() != 2) {
      ++stats.malformed_records;
      stats.binaryfile_completion = "truncated_length";
      std::cerr << "truncated BinaryFILE record length\n";
      break;
    }

    const uint16_t length = readU16BE(length_bytes);
    if (length == 0) {
      stats.bytes += 2;
      ++stats.zero_length_records;
      if (input.peek() != std::char_traits<char>::eof()) {
        ++stats.malformed_records;
        stats.binaryfile_completion = "trailing_data";
        std::cerr
            << "data follows zero-length BinaryFILE terminator\n";
      } else if (!saw_end_of_messages) {
        ++stats.malformed_records;
        stats.binaryfile_completion = "terminator_before_sc";
        std::cerr
            << "BinaryFILE terminator before ITCH System Event C\n";
      } else {
        stats.binaryfile_completion = "terminator";
      }
      break;
    }
    input.read(reinterpret_cast<char *>(message.data()), length);
    if (input.gcount() > 0) {
      binaryfile_digest.update(std::span<const unsigned char>(
          message.data(), static_cast<std::size_t>(input.gcount())));
    }
    if (input.gcount() != static_cast<std::streamsize>(length)) {
      ++stats.malformed_records;
      stats.binaryfile_completion = "truncated_payload";
      std::cerr << "truncated BinaryFILE record payload\n";
      break;
    }

    ++stats.records;
    stats.bytes += static_cast<uint64_t>(length) + 2;

    const std::uint8_t expected_length =
        astra::protocol::expectedItchMessageLength(message[0]);
    if (expected_length == 0 || length != expected_length) {
      ++stats.malformed_records;
      stats.binaryfile_completion = "malformed_record";
      std::cerr << "unsupported ITCH type or invalid message length\n";
      break;
    }

    if (message[0] == 'S' && readU16BE(message.data() + 1) != 0) {
      ++stats.malformed_records;
      stats.binaryfile_completion = "malformed_record";
      std::cerr << "System Event stock locate is not zero\n";
      break;
    }

    const bool is_start_of_messages =
        length == 12 && message[0] == 'S' && message[11] == 'O';
    if (!saw_message && !is_start_of_messages) {
      ++stats.malformed_records;
      stats.binaryfile_completion = "missing_so";
      std::cerr << "first ITCH record is not System Event O\n";
      break;
    }
    if (saw_end_of_messages) {
      ++stats.malformed_records;
      stats.binaryfile_completion = "record_after_sc";
      std::cerr << "ITCH record follows System Event C\n";
      break;
    }

    saw_message = true;
    saw_end_of_messages =
        length == 12 && message[0] == 'S' && message[11] == 'C';
    observeMessage(stats, message.data(), length);
  }

  // Profiles that fail early still report the digest of the entire physical
  // input, while successful terminator/legacy-EOF profiles normally have
  // nothing left to drain. This keeps binaryfile_sha256 unambiguous.
  input.clear();
  std::array<unsigned char, 1024u * 1024u> remaining{};
  while (input.read(reinterpret_cast<char *>(remaining.data()),
                    static_cast<std::streamsize>(remaining.size())) ||
         input.gcount() != 0) {
    binaryfile_digest.update(std::span<const unsigned char>(
        remaining.data(), static_cast<std::size_t>(input.gcount())));
  }
  if (!input.eof()) {
    ++stats.malformed_records;
    stats.binaryfile_completion = "read_error";
    std::cerr << "cannot read complete BinaryFILE for SHA-256\n";
  }
  const std::string binaryfile_sha256 = binaryfile_digest.finalizeHex();

  printStats(stats, argv[1], binaryfile_sha256, profiler_sha256);
  const bool semantic_clean = semanticProfileClean(stats);
  std::cout << "certification semantic_clean="
            << (semantic_clean ? 1 : 0) << '\n';
  if (!semantic_clean)
    std::cerr << "semantic ITCH reconstruction gate failed\n";
  // Flush before destroying the potentially very large reconstruction maps so
  // a full-day run publishes its result immediately after validation.
  std::cout.flush();
  const std::string_view completion(stats.binaryfile_completion);
  const bool complete = completion == "terminator" ||
                        completion == "legacy_sc_eof";
  return stats.malformed_records == 0 && complete && semantic_clean ? 0 : 1;
}
