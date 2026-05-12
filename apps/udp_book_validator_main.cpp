#include "astra/book/BookManager.hpp"
#include "astra/codec/BinaryDecoder.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/sequencing/SequenceTracker.hpp"
#include "astra/source/UdpReceiver.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

uint64_t parseU64(const char *value) {
  return std::strtoull(value, nullptr, 10);
}

uint16_t parsePort(const char *value) {
  return static_cast<uint16_t>(std::strtoul(value, nullptr, 10));
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 4 || argc > 5) {
    std::cerr << "Usage: " << argv[0]
              << " <listen-ip> <port> <expected-packets> [idle-timeout-ms]\n";
    return EXIT_FAILURE;
  }

  const char *ip = argv[1];
  const uint16_t port = parsePort(argv[2]);
  const uint64_t expected_packets = parseU64(argv[3]);
  const uint64_t idle_timeout_ms = argc == 5 ? parseU64(argv[4]) : 10000;

  try {
    UdpReceiver source(ip, port);
    BinaryDecoder decoder;
    SequenceTracker sequence_tracker;
    BookManager books;

    uint64_t packets = 0;
    uint64_t applied = 0;
    uint64_t decode_errors = 0;
    uint64_t sequence_gaps = 0;
    uint64_t duplicate_or_old = 0;

    const auto idle_timeout = std::chrono::milliseconds(idle_timeout_ms);
    auto last_packet_time = std::chrono::steady_clock::now();

    std::cout << "UDP book validator listening on " << ip << ":" << port
              << " expecting " << expected_packets << " packets\n";

    while (packets < expected_packets) {
      PacketView packet;
      if (!source.next(packet)) {
        if (std::chrono::steady_clock::now() - last_packet_time >
            idle_timeout) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      last_packet_time = std::chrono::steady_clock::now();
      ++packets;

      MarketDataMessageView message;
      const DecodeResult decode_result = decoder.decode(packet, message);
      if (decode_result.status != DecodeStatus::Ok) {
        ++decode_errors;
        continue;
      }

      const SequenceStatus sequence_status =
          sequence_tracker.onMessage(message.seqNum());
      if (sequence_status == SequenceStatus::Gap) {
        ++sequence_gaps;
        continue;
      }
      if (sequence_status == SequenceStatus::DuplicateOrOld) {
        ++duplicate_or_old;
        continue;
      }

      books.onMessage(message);
      ++applied;
    }

    std::cout << "packets=" << packets << " applied=" << applied
              << " decode_errors=" << decode_errors
              << " sequence_gaps=" << sequence_gaps
              << " duplicate_or_old=" << duplicate_or_old << "\n";

    return packets == expected_packets && decode_errors == 0 &&
                   sequence_gaps == 0 && duplicate_or_old == 0
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  } catch (const std::exception &e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}
