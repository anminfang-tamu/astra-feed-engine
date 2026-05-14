#include "astra/source/UdpSender.hpp"
#include "replay/SymbolTable.hpp"
#include "replay/itch/ItchReplaySource.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running.store(false, std::memory_order_relaxed); }

uint64_t parseU64(const char *value, uint64_t fallback) {
  if (value == nullptr) {
    return fallback;
  }
  return std::strtoull(value, nullptr, 10);
}

uint16_t parsePort(const char *value) {
  return static_cast<uint16_t>(std::strtoul(value, nullptr, 10));
}

uint8_t parseChannelId(const char *value) {
  return static_cast<uint8_t>(std::strtoul(value, nullptr, 10));
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 4 || argc > 7) {
    std::cerr << "Usage: " << argv[0]
              << " <NASDAQ_ITCH50> <dest-ip> <port> "
                 "[max_messages] [channel_id] [msg/s]\n";
    return EXIT_FAILURE;
  }

  const std::string path = argv[1];
  const char *ip = argv[2];
  const uint16_t port = parsePort(argv[3]);
  const uint64_t max_messages =
      argc >= 5 ? parseU64(argv[4], std::numeric_limits<uint64_t>::max())
                : std::numeric_limits<uint64_t>::max();
  const uint8_t channel_id = argc >= 6 ? parseChannelId(argv[5]) : 0;
  const uint64_t messages_per_second = argc >= 7 ? parseU64(argv[6], 0) : 0;

  try {
    SymbolTable symbols;
    ItchReplaySource source(path, symbols, channel_id);
    if (!source.isOpen()) {
      std::cerr << source.lastError() << "\n";
      return EXIT_FAILURE;
    }

    UdpSender sender(ip, port);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "ITCH replay sender -> " << ip << ":" << port
              << " file=" << path;
    if (messages_per_second > 0) {
      std::cout << " rate=" << messages_per_second << " msg/s";
    } else {
      std::cout << " rate=max";
    }
    std::cout << " -- press Ctrl+C to stop\n";

    const auto interval =
        messages_per_second > 0
            ? std::chrono::nanoseconds(1'000'000'000ULL / messages_per_second)
            : std::chrono::nanoseconds(0);
    auto next_send = std::chrono::steady_clock::now();

    uint64_t consumed = 0;
    uint64_t sent = 0;
    uint64_t send_failures = 0;
    while (g_running.load(std::memory_order_relaxed) && consumed < max_messages) {
      PacketView packet;
      if (!source.next(packet)) {
        break;
      }

      ++consumed;
      if (sender.send(packet.data, packet.size)) {
        ++sent;
      } else {
        ++send_failures;
      }

      if (interval.count() > 0) {
        next_send += interval;
        std::this_thread::sleep_until(next_send);
      }
    }

    const ItchReplayStats &stats = source.stats();
    std::cout << "consumed=" << consumed << " sent=" << sent
              << " send_failures=" << send_failures
              << " records=" << stats.records_read
              << " bytes=" << stats.bytes_read
              << " parsed_events=" << stats.parsed_events
              << " emitted_messages=" << stats.emitted_messages
              << " ignored_events=" << stats.ignored_events
              << " skipped_messages=" << stats.skipped_messages
              << " bad_messages=" << stats.bad_messages
              << " symbols=" << symbols.size() << "\n";

    return stats.bad_messages == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}
