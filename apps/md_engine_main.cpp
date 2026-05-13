#include "astra/book/BookManager.hpp"
#include "astra/codec/BinaryDecoder.hpp"
#include "astra/engine/EngineConfig.hpp"
#include "astra/engine/MarketDataEngine.hpp"
#include "astra/metrics/LatencyRecorder.hpp"
#include "astra/publish/NullPublisher.hpp"
#include "astra/sequencing/SequenceTracker.hpp"
#include "astra/source/UdpReceiver.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>

static MarketDataEngine *g_engine = nullptr;

static void onSignal(int) {
  if (g_engine)
    g_engine->stop();
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <multicast-ip> <port>\n";
    return EXIT_FAILURE;
  }

  const char *ip = argv[1];
  const uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));

  try {
    UdpReceiver source(ip, port);
    BinaryDecoder decoder;
    SequenceTracker sequence_tracker;
    BookManager book_manager;
    NullPublisher publisher;
    LatencyRecorder latency_recorder;
    EngineConfig config;

    MarketDataEngine engine(source, decoder, sequence_tracker, book_manager,
                            publisher, latency_recorder, config);

    g_engine = &engine;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "Engine started on " << ip << ":" << port
              << " — press Ctrl+C to stop\n";

    engine.run();

    std::cout << "Engine stopped\n";
    if (config.enable_latency_metrics) {
      latency_recorder.report();
    }
  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
