#include "astra/codec/BinaryEncoder.hpp"
#include "astra/protocol/MarketDataMessage.hpp"
#include "astra/protocol/WireMessage.hpp"
#include "astra/source/UdpSender.hpp"
#include "simulator/MarketSimulator.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

static std::atomic<bool> g_running{true};

static void onSignal(int) { g_running.store(false, std::memory_order_relaxed); }

int main(int argc, char *argv[]) {
  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " <dest-ip> <port> [msg/s]\n";
    return EXIT_FAILURE;
  }

  const char    *ip   = argv[1];
  const uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
  const uint32_t mps  = (argc == 4) ? static_cast<uint32_t>(std::atoi(argv[3]))
                                    : 1000;

  const auto interval = std::chrono::nanoseconds(1'000'000'000ULL / mps);

  std::signal(SIGINT,  onSignal);
  std::signal(SIGTERM, onSignal);

  try {
    UdpSender       sender(ip, port);
    BinaryEncoder   encoder;
    MarketSimulator sim; // AAPL, base $195.00

    alignas(64) std::byte buf[WireMessageSize];
    MarketDataMessage     msg{};

    std::cout << "AAPL simulator -> " << ip << ":" << port
              << " at " << mps << " msg/s — press Ctrl+C to stop\n";

    uint64_t sent     = 0;
    auto     next_send = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
      sim.next(msg);
      sender.send(buf, encoder.encode(&msg, buf, sizeof(buf)));
      ++sent;

      next_send += interval;
      std::this_thread::sleep_until(next_send);
    }

    std::cout << "Simulator stopped — " << sent << " messages sent"
              << "  (mid price: $" << sim.midPrice() / 100 << "."
              << sim.midPrice() % 100
              << ", book depth: " << sim.bookDepth() << ")\n";
  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
