#include "astra/book/BookManager.hpp"
#include "astra/symbol/StockDirectory.hpp"
#include "astra/parser/ItchParser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

size_t parseSize(const char *name, size_t fallback, size_t minimum,
                 size_t maximum = std::numeric_limits<size_t>::max()) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum || parsed > std::numeric_limits<size_t>::max()) {
    throw std::runtime_error(std::string("invalid ") + name + ": " + value);
  }
  return static_cast<size_t>(parsed);
}

size_t directorySlots() {
  const size_t slots =
      parseSize("ASTRA_ORDER_DIRECTORY_SLOTS",
                OrderRefDirectory::kDefaultSlotCapacity, 2);
  if (!std::has_single_bit(slots)) {
    throw std::runtime_error(
        "ASTRA_ORDER_DIRECTORY_SLOTS must be a power of two");
  }
  return slots;
}

uint16_t readU16BE(const std::array<unsigned char, 2> &bytes) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                               static_cast<uint16_t>(bytes[1]));
}

double elapsedSeconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

bool hasBookFailure(const BookManagerStats &stats) noexcept {
  return stats.invalid_adds != 0 || stats.duplicate_order_refs != 0 ||
         stats.missing_order_refs != 0 || stats.stale_order_refs != 0 ||
         stats.mutation_failures != 0 ||
         stats.directory_insert_failures != 0 ||
         stats.directory_replace_failures != 0 ||
         stats.directory_erase_failures != 0 ||
         stats.late_book_creation_attempts != 0 ||
         stats.allocation_failures != 0 || stats.price_rejections != 0 ||
         stats.locally_invalid_books != 0 ||
         stats.price_levels.internal_node_exhaustions != 0 ||
         stats.price_levels.leaf_exhaustions != 0 ||
         stats.price_levels.level_exhaustions != 0 ||
         stats.orders.invalid_releases != 0 ||
         stats.orders.double_releases != 0 ||
         stats.directory_size != stats.live_orders ||
         stats.orders.in_use != stats.live_orders;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <NASDAQ_ITCH50_file>\n";
    return EXIT_FAILURE;
  }

  try {
    const size_t order_directory_slots = directorySlots();
    const size_t max_order_pool_capacity = std::min(
        order_directory_slots / 2,
        static_cast<size_t>(OrderArena::kInvalidIndex));
    const size_t order_pool_capacity =
        parseSize("ASTRA_ORDER_POOL_CAPACITY", max_order_pool_capacity, 1,
                  max_order_pool_capacity);
    PriceLevelArenaConfig price_config =
        BookManager::defaultPriceLevelArenaConfig(order_pool_capacity);
    price_config.internal_node_capacity = parseSize(
        "ASTRA_PRICE_INTERNAL_NODE_CAPACITY",
        price_config.internal_node_capacity, 2);
    price_config.leaf_capacity =
        parseSize("ASTRA_PRICE_LEAF_CAPACITY", price_config.leaf_capacity, 1);
    price_config.level_capacity =
        parseSize("ASTRA_PRICE_LEVEL_CAPACITY", price_config.level_capacity, 1,
                  Order::kLevelMask);
    const size_t max_records =
        parseSize("ASTRA_REPLAY_MAX_RECORDS", 0, 0);

    const Clock::time_point initialization_start = Clock::now();
    astra::symbol::StockDirectory symbols;
    BookManager books(order_directory_slots, price_config,
                      order_pool_capacity);
    ItchParser parser(symbols, books);
    const Clock::time_point initialization_end = Clock::now();

    std::vector<char> file_buffer(4 * 1024 * 1024);
    std::filebuf input_buffer;
    input_buffer.pubsetbuf(file_buffer.data(),
                           static_cast<std::streamsize>(file_buffer.size()));
    if (input_buffer.open(argv[1], std::ios::in | std::ios::binary) ==
        nullptr) {
      throw std::runtime_error(std::string("failed to open ") + argv[1]);
    }
    std::istream input(&input_buffer);

    std::array<uint64_t, 256> message_counts{};
    std::vector<std::byte> message(65'535);
    std::array<unsigned char, 2> length_bytes{};
    uint64_t records = 0;
    uint64_t parser_errors = 0;
    uint64_t bytes = 0;

    const Clock::time_point replay_start = Clock::now();
    for (;;) {
      if (max_records != 0 && records >= max_records) {
        break;
      }
      input.read(reinterpret_cast<char *>(length_bytes.data()), 2);
      if (input.gcount() == 0 && input.eof()) {
        break;
      }
      if (input.gcount() != 2) {
        throw std::runtime_error("truncated ITCH record length");
      }

      const uint16_t length = readU16BE(length_bytes);
      if (length == 0) {
        throw std::runtime_error("zero-length ITCH record");
      }
      input.read(reinterpret_cast<char *>(message.data()), length);
      if (input.gcount() != static_cast<std::streamsize>(length)) {
        throw std::runtime_error("truncated ITCH record payload");
      }

      ++records;
      bytes += static_cast<uint64_t>(length) + 2;
      const uint8_t type = std::to_integer<uint8_t>(message[0]);
      ++message_counts[type];
      (void)parser.handleMessage(
          std::span<const std::byte>(message.data(), length));
      if (!parser.lastError().empty()) {
        ++parser_errors;
      }
    }
    const Clock::time_point replay_end = Clock::now();

    const BookManagerStats stats = books.stats();
    uint64_t top_checksum = 0;
    for (uint16_t locate = 1; locate < BookManager::kMaxStockLocate;
         ++locate) {
      const OrderBook *book = books.getOrderBook(locate);
      if (book == nullptr) {
        continue;
      }
      const TopOfBook top = book->getTopOfBook();
      top_checksum ^= top.bid_price + (top.bid_qty << 1) +
                      (top.ask_price << 2) + (top.ask_qty << 3) + locate;
    }

    const double initialization_seconds =
        elapsedSeconds(initialization_start, initialization_end);
    const double replay_seconds = elapsedSeconds(replay_start, replay_end);
    const double records_per_second =
        replay_seconds == 0 ? 0 : static_cast<double>(records) / replay_seconds;
    const double gib_per_second =
        replay_seconds == 0
            ? 0
            : static_cast<double>(bytes) /
                  (1024.0 * 1024.0 * 1024.0 * replay_seconds);

    const bool passed = parser_errors == 0 && !hasBookFailure(stats);
    std::cout << std::fixed << std::setprecision(3)
              << "itch_replay_benchmark status="
              << (passed ? "pass" : "fail") << " records=" << records
              << " bytes=" << bytes
              << " initialization_seconds=" << initialization_seconds
              << " replay_seconds=" << replay_seconds
              << " records_per_second=" << records_per_second
              << " gib_per_second=" << gib_per_second
              << " parser_errors=" << parser_errors
              << " symbols=" << symbols.size()
              << " books=" << stats.books << " live_orders=" << stats.live_orders
              << " top_checksum=" << top_checksum << '\n';

    std::cout << "book_gate directory_size=" << stats.directory_size
              << " directory_slots=" << stats.directory_capacity
              << " directory_max_entries=" << stats.directory_max_entries
              << " directory_max_probe=" << stats.directory_max_probe_length
              << " invalid_adds=" << stats.invalid_adds
              << " duplicate_order_refs=" << stats.duplicate_order_refs
              << " missing_order_refs=" << stats.missing_order_refs
              << " stale_order_refs=" << stats.stale_order_refs
              << " mutation_failures=" << stats.mutation_failures
              << " directory_insert_failures="
              << stats.directory_insert_failures
              << " directory_replace_failures="
              << stats.directory_replace_failures
              << " directory_erase_failures="
              << stats.directory_erase_failures
              << " late_book_creation_attempts="
              << stats.late_book_creation_attempts
              << " allocation_failures=" << stats.allocation_failures
              << " order_pool_in_use=" << stats.orders.in_use
              << " order_pool_capacity=" << stats.orders.capacity
              << " order_pool_high_watermark="
              << stats.orders.high_watermark
              << " order_pool_invalid_releases="
              << stats.orders.invalid_releases
              << " order_pool_double_releases="
              << stats.orders.double_releases
              << " price_rejections=" << stats.price_rejections
              << " locally_invalid_books=" << stats.locally_invalid_books
              << '\n';

    std::cout << "price_pool_gate internal_nodes="
              << stats.price_levels.internal_nodes_in_use << '/'
              << stats.price_levels.internal_node_capacity
              << " internal_node_high_watermark="
              << stats.price_levels.internal_node_high_watermark
              << " leaves=" << stats.price_levels.leaves_in_use << '/'
              << stats.price_levels.leaf_capacity
              << " leaf_high_watermark="
              << stats.price_levels.leaf_high_watermark
              << " levels=" << stats.price_levels.levels_in_use << '/'
              << stats.price_levels.level_capacity
              << " level_high_watermark="
              << stats.price_levels.level_high_watermark
              << " internal_node_exhaustions="
              << stats.price_levels.internal_node_exhaustions
              << " leaf_exhaustions="
              << stats.price_levels.leaf_exhaustions
              << " level_exhaustions="
              << stats.price_levels.level_exhaustions << '\n';

    std::cout << "message_counts";
    for (size_t type = 0; type < message_counts.size(); ++type) {
      if (message_counts[type] != 0) {
        std::cout << ' ' << static_cast<char>(type) << '='
                  << message_counts[type];
      }
    }
    std::cout << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception &error) {
    std::cerr << "Fatal: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
