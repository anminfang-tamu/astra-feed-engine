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
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running.store(false, std::memory_order_relaxed); }

uint64_t parseU64(const char *value, uint64_t fallback) {
    if (value == nullptr) return fallback;
    return std::strtoull(value, nullptr, 10);
}

double parseDouble(const char *value, double fallback) {
    if (value == nullptr) return fallback;
    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || parsed <= 0.0) return fallback;
    return parsed;
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

uint64_t readU48BE(const std::byte *p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i)
        v = (v << 8) | std::to_integer<uint8_t>(p[i]);
    return v;
}

uint64_t readU48BE(const char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i)
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    return v;
}

enum class PremarketReplayMode {
    Off,
    Flat,
    Timestamp,
};

PremarketReplayMode parseReplayMode(const char *value,
                                    uint64_t premarket_seconds) {
    if (value == nullptr || value[0] == '\0')
        return premarket_seconds > 0 ? PremarketReplayMode::Flat
                                     : PremarketReplayMode::Off;

    const std::string_view mode(value);
    if (mode == "flat") return PremarketReplayMode::Flat;
    if (mode == "timestamp") return PremarketReplayMode::Timestamp;
    if (mode == "off" || mode == "none" || mode == "disabled")
        return PremarketReplayMode::Off;

    return premarket_seconds > 0 ? PremarketReplayMode::Flat
                                 : PremarketReplayMode::Off;
}

const char *replayModeName(PremarketReplayMode mode) {
    switch (mode) {
    case PremarketReplayMode::Flat: return "flat";
    case PremarketReplayMode::Timestamp: return "timestamp";
    case PremarketReplayMode::Off: return "off";
    }
    return "off";
}

struct PacketTiming {
    bool     has_timestamp{false};
    uint64_t first_timestamp_ns{0};
    bool     contains_ss{false};
    bool     contains_sq{false};
    uint64_t ss_timestamp_ns{0};
    uint64_t sq_timestamp_ns{0};
};

PacketTiming inspectPacket(const PacketView &packet) {
    PacketTiming timing;
    if (packet.size < ItchMoldUdpSource::kMoldHeaderSize) return timing;

    const uint16_t msg_count = readU16BE(packet.data + 18);
    std::size_t offset = ItchMoldUdpSource::kMoldHeaderSize;
    for (uint16_t i = 0; i < msg_count; ++i) {
        if (offset + 2 > packet.size) return timing;
        const uint16_t msg_len = readU16BE(packet.data + offset);
        offset += 2;
        if (msg_len == 0 || offset + msg_len > packet.size) return timing;

        const std::byte *msg = packet.data + offset;
        if (msg_len >= 11 && !timing.has_timestamp) {
            timing.first_timestamp_ns = readU48BE(msg + 5);
            timing.has_timestamp = true;
        }

        if (msg_len >= 12 &&
            static_cast<char>(std::to_integer<uint8_t>(msg[0])) == 'S') {
            const char event_code =
                static_cast<char>(std::to_integer<uint8_t>(msg[11]));
            if (event_code == 'S') {
                timing.contains_ss = true;
                timing.ss_timestamp_ns = readU48BE(msg + 5);
            } else if (event_code == 'Q') {
                timing.contains_sq = true;
                timing.sq_timestamp_ns = readU48BE(msg + 5);
            }
        }
        offset += msg_len;
    }
    return timing;
}

struct PremarketWindow {
    bool     found{false};
    uint64_t ss_packet{0};
    uint64_t sq_packet{0};
    uint64_t ss_timestamp_ns{0};
    uint64_t sq_timestamp_ns{0};

    uint64_t packetsToPace() const {
        if (!found || sq_packet <= ss_packet) return 0;
        return sq_packet - ss_packet;
    }

    uint64_t durationNs() const {
        if (!found || sq_timestamp_ns <= ss_timestamp_ns) return 0;
        return sq_timestamp_ns - ss_timestamp_ns;
    }
};

PremarketWindow scanPremarketWindow(const std::string &path,
                                    uint16_t msgs_per_packet) {
    PremarketWindow window;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return window;

    uint64_t packet_index = 1;
    uint16_t packet_count = 0;
    std::size_t packet_offset = ItchMoldUdpSource::kMoldHeaderSize;
    std::vector<char> msg;

    while (input) {
        unsigned char len_bytes[2];
        input.read(reinterpret_cast<char *>(len_bytes), 2);
        if (input.gcount() == 0 && input.eof()) break;
        if (input.gcount() != 2) break;

        const uint16_t msg_len =
            (static_cast<uint16_t>(len_bytes[0]) << 8) | len_bytes[1];
        if (msg_len == 0) break;

        if (packet_count == msgs_per_packet ||
            packet_offset + 2u + msg_len > ItchMoldUdpSource::kMaxPacketBytes) {
            ++packet_index;
            packet_count = 0;
            packet_offset = ItchMoldUdpSource::kMoldHeaderSize;
        }

        msg.resize(msg_len);
        input.read(msg.data(), static_cast<std::streamsize>(msg.size()));
        if (input.gcount() != static_cast<std::streamsize>(msg.size()))
            return window;

        const char type = msg.empty() ? '\0' : msg[0];
        const char event_code = msg.size() > 11 ? msg[11] : '\0';
        const uint64_t timestamp_ns =
            msg.size() >= 11 ? readU48BE(msg.data() + 5) : 0;

        ++packet_count;
        packet_offset += 2u + msg_len;

        if (type == 'S' && event_code == 'S') {
            window.ss_packet = packet_index;
            window.ss_timestamp_ns = timestamp_ns;
        } else if (type == 'S' && event_code == 'Q' && window.ss_packet != 0) {
            window.sq_packet = packet_index;
            window.sq_timestamp_ns = timestamp_ns;
            window.found = true;
            return window;
        }
    }

    return window;
}

std::chrono::nanoseconds intervalFor(uint64_t seconds, uint64_t packets) {
    if (seconds == 0 || packets == 0) return std::chrono::nanoseconds(0);

    const long double total_ns =
        static_cast<long double>(seconds) * 1'000'000'000.0L;
    const long double interval_ns = total_ns / static_cast<long double>(packets);
    if (interval_ns < 1.0L) return std::chrono::nanoseconds(1);
    if (interval_ns >
        static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::chrono::nanoseconds(std::numeric_limits<int64_t>::max());
    }
    return std::chrono::nanoseconds(static_cast<int64_t>(interval_ns));
}

std::chrono::nanoseconds scaledDelay(uint64_t ns_delta, double speedup) {
    if (ns_delta == 0 || speedup <= 0.0) return std::chrono::nanoseconds(0);

    const long double scaled_ns =
        static_cast<long double>(ns_delta) / static_cast<long double>(speedup);
    if (scaled_ns < 1.0L) return std::chrono::nanoseconds(0);
    if (scaled_ns >
        static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::chrono::nanoseconds(std::numeric_limits<int64_t>::max());
    }
    return std::chrono::nanoseconds(static_cast<int64_t>(scaled_ns));
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 4 || argc > 11) {
        std::cerr << "Usage: " << argv[0]
                  << " <NASDAQ_ITCH50> <dest-ip> <port>"
                     " [msgs_per_packet] [session] [pkt/s] [premarket_seconds]"
                     " [ss_pause_seconds] [premarket_replay_mode]"
                     " [premarket_speedup]\n"
                  << "  msgs_per_packet  ITCH messages bundled per UDP datagram"
                     " (default " << ItchMoldUdpSource::kDefaultMsgsPerPacket << ")\n"
                  << "  session          10-char MoldUDP64 session id"
                     " (default \"ASTRA     \")\n"
                  << "  pkt/s            packet rate limit; 0 = max speed\n"
                  << "  premarket_seconds"
                     "  stretch traffic after SS until SQ to this duration;"
                     " 0 = disabled (default, or ASTRA_PREMARKET_SECONDS)\n"
                  << "  ss_pause_seconds"
                     "  pause after sending SS before premarket pacing starts;"
                     " 0 = disabled (default, or ASTRA_SS_PAUSE_SECONDS)\n"
                  << "  premarket_replay_mode"
                     "  flat|timestamp|off"
                     " (default ASTRA_PREMARKET_REPLAY_MODE, else flat when"
                     " premarket_seconds > 0)\n"
                  << "  premarket_speedup"
                     "  timestamp mode speedup factor"
                     " (default ASTRA_PREMARKET_SPEEDUP or 1.0)\n";
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
    const uint64_t    premarket_seconds =
        argc >= 8 ? parseU64(argv[7], 0)
                  : parseU64(std::getenv("ASTRA_PREMARKET_SECONDS"), 0);
    const uint64_t    ss_pause_seconds =
        argc >= 9 ? parseU64(argv[8], 0)
                  : parseU64(std::getenv("ASTRA_SS_PAUSE_SECONDS"), 0);
    const PremarketReplayMode replay_mode =
        parseReplayMode(argc >= 10 ? argv[9]
                                   : std::getenv("ASTRA_PREMARKET_REPLAY_MODE"),
                        premarket_seconds);
    const double premarket_speedup =
        argc >= 11 ? parseDouble(argv[10], 1.0)
                   : parseDouble(std::getenv("ASTRA_PREMARKET_SPEEDUP"), 1.0);

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

        const bool needs_premarket_window =
            replay_mode == PremarketReplayMode::Flat ||
            replay_mode == PremarketReplayMode::Timestamp;
        const PremarketWindow premarket =
            needs_premarket_window ? scanPremarketWindow(path, msgs_per_packet)
                                   : PremarketWindow{};
        const auto premarket_interval =
            replay_mode == PremarketReplayMode::Flat
                ? intervalFor(premarket_seconds, premarket.packetsToPace())
                : std::chrono::nanoseconds(0);
        const bool premarket_enabled =
            (replay_mode == PremarketReplayMode::Flat &&
             premarket_interval.count() > 0) ||
            (replay_mode == PremarketReplayMode::Timestamp && premarket.found &&
             premarket.durationNs() > 0 && premarket_speedup > 0.0);

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
        if (replay_mode != PremarketReplayMode::Off) {
            std::cout << "  premarket_mode=" << replayModeName(replay_mode);
            if (premarket_enabled) {
                std::cout << " premarket_packets=" << premarket.packetsToPace();
                if (replay_mode == PremarketReplayMode::Flat) {
                    std::cout << " premarket_seconds=" << premarket_seconds;
                } else {
                    const long double real_seconds =
                        static_cast<long double>(premarket.durationNs()) /
                        1'000'000'000.0L;
                    std::cout << " premarket_speedup=" << premarket_speedup
                              << " premarket_real_seconds="
                              << static_cast<double>(real_seconds)
                              << " premarket_scaled_seconds="
                              << static_cast<double>(
                                     real_seconds / premarket_speedup);
                }
            } else {
                std::cout << " premarket_window=not-found";
            }
        }
        if (ss_pause_seconds > 0)
            std::cout << "  ss_pause_seconds=" << ss_pause_seconds;
        std::cout << "\n  (press Ctrl+C to stop)\n";

        const auto interval =
            pkts_per_second > 0
                ? std::chrono::nanoseconds(1'000'000'000ULL / pkts_per_second)
                : std::chrono::nanoseconds(0);
        auto next_send = std::chrono::steady_clock::now();
        auto next_premarket_send = std::chrono::steady_clock::now();
        auto premarket_start = std::chrono::steady_clock::now();
        bool premarket_active = false;

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
            const PacketTiming packet_timing =
                premarket_enabled ? inspectPacket(packet) : PacketTiming{};
            const bool contains_ss =
                premarket_enabled && packet_timing.contains_ss;
            const bool contains_sq =
                premarket_enabled && packet_timing.contains_sq;

            if (premarket_active) {
                if (replay_mode == PremarketReplayMode::Flat) {
                    std::this_thread::sleep_until(next_premarket_send);
                    next_premarket_send += premarket_interval;
                } else if (replay_mode == PremarketReplayMode::Timestamp) {
                    uint64_t packet_timestamp = packet_timing.first_timestamp_ns;
                    if (packet_timing.contains_sq)
                        packet_timestamp = packet_timing.sq_timestamp_ns;
                    if (packet_timing.has_timestamp &&
                        packet_timestamp > premarket.ss_timestamp_ns) {
                        std::this_thread::sleep_until(
                            premarket_start +
                            scaledDelay(packet_timestamp -
                                            premarket.ss_timestamp_ns,
                                        premarket_speedup));
                    }
                }
            }

            if (sender.send(packet.data, packet.size)) {
                ++pkts_sent;
                msgs_sent += msg_count;
                if (first_seq_sent == 0)
                    first_seq_sent = first_seq;
                next_seq_sent = first_seq + msg_count;
            } else {
                ++send_failures;
            }

            if (contains_ss && !premarket_active) {
                if (ss_pause_seconds > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::seconds(ss_pause_seconds));
                }
                premarket_active = true;
                premarket_start = std::chrono::steady_clock::now();
                next_premarket_send = premarket_start + premarket_interval;
            }

            if (contains_sq) {
                premarket_active = false;
                next_send = std::chrono::steady_clock::now();
            }

            if (!premarket_active && !contains_ss && !contains_sq &&
                interval.count() > 0) {
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
