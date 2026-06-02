#include "astra/source/UdpSender.hpp"
#include "astra/utils/CpuAffinity.hpp"
#include "replay/itch/ItchMoldUdpSource.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running.store(false, std::memory_order_relaxed); }

uint64_t parseU64(const char *value, uint64_t fallback) {
    if (value == nullptr) return fallback;
    return std::strtoull(value, nullptr, 10);
}

uint16_t parsePort(const char *value) {
    return static_cast<uint16_t>(std::strtoul(value, nullptr, 10));
}

uint16_t parseMsgsPerPacket(const char *value) {
    return static_cast<uint16_t>(
        parseU64(value, ItchMoldUdpSource::kDefaultMsgsPerPacket));
}

uint16_t readU16BE(const std::byte *p) {
    return (static_cast<uint16_t>(std::to_integer<uint8_t>(p[0])) << 8) |
           static_cast<uint16_t>(std::to_integer<uint8_t>(p[1]));
}

uint64_t readU64BE(const std::byte *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | std::to_integer<uint8_t>(p[i]);
    return v;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 4 || argc > 7) {
        std::cerr << "Usage: " << argv[0]
                  << " <NASDAQ_ITCH50> <dest-ip> <port>"
                     " [msgs_per_packet] [session] [pkt/s]\n"
                  << "  msgs_per_packet  ITCH messages bundled per UDP datagram"
                     " (default " << ItchMoldUdpSource::kDefaultMsgsPerPacket << ")\n"
                  << "  session          10-char MoldUDP64 session id"
                     " (default \"ASTRA     \")\n"
                  << "  pkt/s            packet rate limit; 0 = max speed\n";
        return EXIT_FAILURE;
    }

    const std::string path  = argv[1];
    const char       *ip    = argv[2];
    const uint16_t    port  = parsePort(argv[3]);
    const uint16_t    msgs_per_packet =
        argc >= 5 ? parseMsgsPerPacket(argv[4])
                  : ItchMoldUdpSource::kDefaultMsgsPerPacket;
    const std::string session        = argc >= 6 ? argv[5] : "ASTRA     ";
    const uint64_t    pkts_per_second = argc >= 7 ? parseU64(argv[6], 0) : 0;

    try {
        if (const char *cpu_env = std::getenv("ASTRA_CPU")) {
            const int cpu_id =
                static_cast<int>(std::strtol(cpu_env, nullptr, 10));
            const auto affinity = astra::utils::pinCurrentThreadToCpu(cpu_id);
            std::cout << "cpu_affinity status="
                      << astra::utils::cpuAffinityStatusName(affinity.status)
                      << " cpu=" << affinity.cpu_id;
            if (!affinity)
                std::cout << " error=" << affinity.error_code
                          << " message=\"" << affinity.message << "\"";
            std::cout << "\n";
        }

        ItchMoldUdpSource source(path, session, msgs_per_packet);
        if (!source.isOpen()) {
            std::cerr << source.lastError() << "\n";
            return EXIT_FAILURE;
        }

        UdpSender sender(ip, port);
        std::signal(SIGINT,  onSignal);
        std::signal(SIGTERM, onSignal);

        std::cout << "MoldUDP64/ITCH5.0 sender -> " << ip << ":" << port
                  << "  file=" << path
                  << "  session=" << session
                  << "  msgs_per_packet=" << msgs_per_packet;
        if (pkts_per_second > 0)
            std::cout << "  rate=" << pkts_per_second << " pkt/s";
        else
            std::cout << "  rate=max";
        std::cout << "\n  (press Ctrl+C to stop)\n";

        const auto interval =
            pkts_per_second > 0
                ? std::chrono::nanoseconds(1'000'000'000ULL / pkts_per_second)
                : std::chrono::nanoseconds(0);
        auto next_send = std::chrono::steady_clock::now();

        uint64_t  pkts_sent     = 0;
        uint64_t  msgs_sent     = 0;
        uint64_t  send_failures = 0;
        uint64_t  first_seq_sent = 0;
        uint64_t  next_seq_sent  = 0;
        PacketView packet;

        while (g_running.load(std::memory_order_relaxed)) {
            if (!source.next(packet)) break;

            const uint64_t first_seq = readU64BE(packet.data + 10);
            const uint16_t msg_count = readU16BE(packet.data + 18);

            if (sender.send(packet.data, packet.size)) {
                ++pkts_sent;
                msgs_sent += msg_count;
                if (first_seq_sent == 0)
                    first_seq_sent = first_seq;
                next_seq_sent = first_seq + msg_count;
            } else {
                ++send_failures;
            }

            if (interval.count() > 0) {
                next_send += interval;
                std::this_thread::sleep_until(next_send);
            }
        }

        std::cout << "pkts_sent=" << pkts_sent
                  << " msgs_sent=" << msgs_sent
                  << " first_seq=" << first_seq_sent
                  << " next_seq=" << next_seq_sent
                  << " send_failures=" << send_failures << "\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
