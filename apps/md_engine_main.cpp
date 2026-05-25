#include "astra/book/BookManager.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/engine/EngineConfig.hpp"
#include "astra/engine/MarketDataEngine.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
#include "astra/metrics/StageLatencyRecorder.hpp"
#include "astra/protocol/SymbolTable.hpp"
#include "astra/source/DualUdpReceiver.hpp"
#include "astra/source/UdpBachReceiver.hpp"
#include "astra/source/UdpReceiver.hpp"

#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>

static MarketDataEngine *g_engine = nullptr;

static void onSignal(int) {
  if (g_engine)
    g_engine->stop();
}

int main(int argc, char *argv[]) {
  if (argc != 3 && argc != 4 && argc != 5 && argc != 6) {
    std::cerr
        << "Usage:\n"
        << "  " << argv[0] << " <ip> <port> [channel_id]\n"
        << "  " << argv[0]
        << " <ip_a> <port_a> <ip_b> <port_b> [channel_id]\n"
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
    const char *rx_mode = std::getenv("ASTRA_UDP_RX");
    const bool use_recvmmsg =
        !dual_feed && rx_mode != nullptr &&
        (std::strcmp(rx_mode, "recvmmsg") == 0 ||
         std::strcmp(rx_mode, "batch") == 0 ||
         std::strcmp(rx_mode, "bach") == 0);
    std::size_t batch_size = UdpBachReceiver::kDefaultBatchSize;
    if (const char *batch_env = std::getenv("ASTRA_UDP_BATCH_SIZE")) {
      const auto parsed = std::strtoul(batch_env, nullptr, 10);
      if (parsed > 0)
        batch_size = static_cast<std::size_t>(parsed);
    }

    SymbolTable symbols;
    BookManager book_manager;
    MoldUdpDecoder decoder(symbols, book_manager, channel_id);
    std::unique_ptr<IMarketDataSource> source;
    if (dual_feed) {
      source = std::make_unique<DualUdpReceiver>(
          argv[1], static_cast<uint16_t>(std::atoi(argv[2])), argv[3],
          static_cast<uint16_t>(std::atoi(argv[4])));
    } else if (use_recvmmsg) {
      source = std::make_unique<UdpBachReceiver>(ip, port, batch_size);
    } else {
      source = std::make_unique<UdpReceiver>(ip, port);
    }
    LatencyRecorder latency_recorder;
    StageLatencyRecorder stage_latency_recorder;
    EngineConfig config;
    decoder.setStageTimingEnabled(config.enable_latency_metrics);

    MarketDataEngine engine(*source, decoder, latency_recorder,
                            &stage_latency_recorder, config);

    g_engine = &engine;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (dual_feed) {
      std::cout << "Engine started  line_a=" << argv[1] << ":" << argv[2]
                << "  line_b=" << argv[3] << ":" << argv[4]
                << "  channel=" << static_cast<int>(channel_id)
                << " — press Ctrl+C to stop\n";
    } else {
      std::cout << "Engine started  addr=" << ip << ":" << port
                << "  channel=" << static_cast<int>(channel_id)
                << "  rx=" << (use_recvmmsg ? "recvmmsg" : "recv")
                << "  batch_size=" << (use_recvmmsg ? batch_size : 1)
                << " — press Ctrl+C to stop\n";
    }

    engine.run();

    std::cout << "Engine stopped  symbols=" << symbols.size() << "\n";
    if (config.enable_latency_metrics) {
      latency_recorder.report();
      stage_latency_recorder.report();
    }

  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
