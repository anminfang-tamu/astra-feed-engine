#include "astra/book/BookManager.hpp"
#include "astra/codec/MoldUdpDecoder.hpp"
#include "astra/engine/EngineConfig.hpp"
#include "astra/engine/MarketDataEngine.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
#include "astra/protocol/SymbolTable.hpp"
#include "astra/source/UdpReceiver.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>

static MarketDataEngine *g_engine = nullptr;

static void onSignal(int) {
  if (g_engine)
    g_engine->stop();
}

int main(int argc, char *argv[]) {
  if (argc < 3 || argc > 4) {
    std::cerr
        << "Usage: " << argv[0] << " <ip> <port> [channel_id]\n"
        << "  Receives MoldUDP64/ITCH 5.0 and builds a live order book.\n";
    return EXIT_FAILURE;
  }

  const char *ip = argv[1];
  const uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
  const uint8_t channel_id =
      argc == 4 ? static_cast<uint8_t>(std::strtoul(argv[3], nullptr, 10)) : 0;

  try {
    SymbolTable symbols;
    BookManager book_manager;
    MoldUdpDecoder decoder(symbols, book_manager, channel_id);
    UdpReceiver source(ip, port);
    LatencyRecorder latency_recorder;
    EngineConfig config;

    MarketDataEngine engine(source, decoder, latency_recorder, config);

    g_engine = &engine;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "Engine started  addr=" << ip << ":" << port
              << "  channel=" << static_cast<int>(channel_id)
              << " — press Ctrl+C to stop\n";

    engine.run();

    std::cout << "Engine stopped  symbols=" << symbols.size() << "\n";
    if (config.enable_latency_metrics)
      latency_recorder.report();

  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
