#include "astra/protocol/PacketHeader.hpp"
#include "astra/protocol/MoldUdpControlPacket.hpp"
#include "astra/protocol/MoldUdpControlSchedule.hpp"
#include "astra/source/UdpSender.hpp"
#include "astra/utils/CpuAffinity.hpp"
#include "replay/itch/ItchMoldUdpSource.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_running{true};
constexpr uint64_t kDefaultLineBDelayNs = 1'000;
constexpr uint64_t kMaxLineBDelayNs = 1'000'000'000;
constexpr uint64_t kDefaultStartupHeartbeatCount = 0;
constexpr uint64_t kDefaultStartupHeartbeatIntervalMs = 1'000;
constexpr uint64_t kMaxStartupHeartbeatCount = 1'000'000;
constexpr uint64_t kMaxStartupHeartbeatIntervalMs = 60'000;
constexpr uint64_t kDefaultIdleHeartbeatIntervalMs = 1'000;
constexpr uint64_t kMaxIdleHeartbeatIntervalMs = 60'000;
constexpr uint64_t kDefaultEndOfSessionPacketCount = 10;
constexpr uint64_t kDefaultEndOfSessionIntervalMs = 100;
constexpr uint64_t kMaxEndOfSessionPacketCount = 1'000'000;
constexpr uint64_t kMaxEndOfSessionIntervalMs = 60'000;
constexpr uint64_t kMaxPacketsPerSecond =
    astra::protocol::MoldUdpPacketRateSchedule::kMaximumPacketsPerSecond;
constexpr uint64_t kMaxSchedulingSeconds = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::duration::max())
        .count());

void onSignal(int) { g_running.store(false, std::memory_order_relaxed); }

bool sleepUntilOrStopped(std::chrono::steady_clock::time_point deadline) {
    constexpr auto kMaxSleep = std::chrono::milliseconds(100);
    while (g_running.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return true;
        const auto remaining = deadline - now;
        const auto max_sleep =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                kMaxSleep);
        std::this_thread::sleep_for(remaining < max_sleep ? remaining
                                                          : max_sleep);
    }
    return false;
}

std::chrono::steady_clock::time_point checkedDeadlineAfter(
    std::chrono::steady_clock::time_point base,
    std::chrono::steady_clock::duration delay,
    const char *label) {
    if (delay < std::chrono::steady_clock::duration::zero() ||
        base > std::chrono::steady_clock::time_point::max() - delay) {
        throw std::out_of_range(std::string(label) +
                                " exceeds steady-clock range");
    }
    return base + delay;
}

bool parseU64Strict(const char *value, uint64_t &parsed_value) {
    if (value == nullptr || value[0] == '\0') return false;

    uint64_t parsed = 0;
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(*p - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
            return false;
        parsed = parsed * 10u + digit;
    }
    parsed_value = parsed;
    return true;
}

bool parseCpuId(const char *value, int &cpu_id) {
    uint64_t parsed = 0;
    if (!parseU64Strict(value, parsed) ||
        parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    cpu_id = static_cast<int>(parsed);
    return true;
}

bool parseU64InRange(const char *value,
                     uint64_t minimum,
                     uint64_t maximum,
                     uint64_t &parsed_value) {
    uint64_t parsed = 0;
    if (!parseU64Strict(value, parsed) || parsed < minimum ||
        parsed > maximum) {
        return false;
    }
    parsed_value = parsed;
    return true;
}

bool parsePositiveDoubleStrict(const char *value, double &parsed_value) {
    if (value == nullptr || value[0] == '\0') return false;

    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || errno == ERANGE ||
        !std::isfinite(parsed) || parsed <= 0.0) {
        return false;
    }
    parsed_value = parsed;
    return true;
}

bool parsePort(const char *value, uint16_t &port) {
    uint64_t parsed = 0;
    if (!parseU64InRange(
            value, 1, std::numeric_limits<uint16_t>::max(), parsed)) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

bool parseMsgsPerPacket(const char *value, uint16_t &msgs_per_packet) {
    uint64_t parsed = 0;
    if (!parseU64InRange(
            value, 1, std::numeric_limits<uint16_t>::max(), parsed)) {
        return false;
    }
    msgs_per_packet = static_cast<uint16_t>(parsed);
    return true;
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

bool parseReplayMode(const char *value,
                     uint64_t premarket_seconds,
                     PremarketReplayMode &mode) {
    if (value == nullptr || value[0] == '\0') {
        mode = premarket_seconds > 0 ? PremarketReplayMode::Flat
                                     : PremarketReplayMode::Off;
        return true;
    }

    const std::string_view value_view(value);
    if (value_view == "flat") {
        mode = PremarketReplayMode::Flat;
        return true;
    }
    if (value_view == "timestamp") {
        mode = PremarketReplayMode::Timestamp;
        return true;
    }
    if (value_view == "off" || value_view == "none" ||
        value_view == "disabled") {
        mode = PremarketReplayMode::Off;
        return true;
    }

    return false;
}

const char *replayModeName(PremarketReplayMode mode) {
    switch (mode) {
    case PremarketReplayMode::Flat: return "flat";
    case PremarketReplayMode::Timestamp: return "timestamp";
    case PremarketReplayMode::Off: return "off";
    }
    return "off";
}

const char *completionKindName(
    ItchMoldUdpSource::CompletionKind kind) noexcept {
    switch (kind) {
    case ItchMoldUdpSource::CompletionKind::None:
        return "none";
    case ItchMoldUdpSource::CompletionKind::Terminator:
        return "terminator";
    case ItchMoldUdpSource::CompletionKind::LegacyEndOfMessagesEof:
        return "legacy_sc_eof";
    }
    return "none";
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
    bool force_next_packet = false;
    std::vector<char> msg;

    while (input) {
        unsigned char len_bytes[2];
        input.read(reinterpret_cast<char *>(len_bytes), 2);
        if (input.gcount() == 0 && input.eof()) break;
        if (input.gcount() != 2) break;

        const uint16_t msg_len =
            (static_cast<uint16_t>(len_bytes[0]) << 8) | len_bytes[1];
        if (msg_len == 0) break;

        if (force_next_packet || packet_count == msgs_per_packet ||
            packet_offset + 2u + msg_len > ItchMoldUdpSource::kMaxPacketBytes) {
            ++packet_index;
            packet_count = 0;
            packet_offset = ItchMoldUdpSource::kMoldHeaderSize;
            force_next_packet = false;
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
            force_next_packet = true;
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

    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000;
    if (seconds >
        std::numeric_limits<uint64_t>::max() / kNanosecondsPerSecond) {
        return std::chrono::nanoseconds(std::numeric_limits<int64_t>::max());
    }
    const uint64_t total_ns = seconds * kNanosecondsPerSecond;
    const uint64_t whole_ns = total_ns / packets;
    const uint64_t remainder = total_ns % packets;
    const uint64_t ceiling_ns = whole_ns + (remainder == 0 ? 0u : 1u);
    if (ceiling_ns >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::chrono::nanoseconds(std::numeric_limits<int64_t>::max());
    }
    return std::chrono::nanoseconds(static_cast<int64_t>(ceiling_ns));
}

std::chrono::nanoseconds scaledDelay(uint64_t ns_delta, double speedup) {
    if (ns_delta == 0 || speedup <= 0.0) return std::chrono::nanoseconds(0);

    const long double scaled_ns =
        static_cast<long double>(ns_delta) / static_cast<long double>(speedup);
    if (scaled_ns < 1.0L) return std::chrono::nanoseconds(1);
    if (scaled_ns >
        static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::chrono::nanoseconds(std::numeric_limits<int64_t>::max());
    }
    return std::chrono::nanoseconds(
        static_cast<int64_t>(std::ceil(scaled_ns)));
}

struct RedundantPacketHandoff {
    const std::byte *data{nullptr};
    std::size_t      size{0};
    uint16_t         msg_count{0};
    std::chrono::steady_clock::time_point line_b_not_before{};
    bool             line_b_send_succeeded{false};

    // A completes before generation is published. The producer cannot reuse
    // the source buffer until B acknowledges that generation.
    alignas(64) std::atomic<uint64_t> generation{0};
    alignas(64) std::atomic<uint64_t> line_b_completed_generation{0};
    alignas(64) std::atomic<bool> stop{false};
};

struct RedundantLineState {
    // 0 = starting, 1 = ready, 2 = failed.
    std::atomic<unsigned> startup_status{0};
    astra::utils::CpuAffinityResult affinity{};
    std::string startup_error;

    uint64_t packets_sent{0};
    uint64_t messages_sent{0};
    uint64_t send_failures{0};
    uint64_t delay_overruns{0};
};

class RedundantUdpSender {
public:
    RedundantUdpSender(std::string ip,
                       uint16_t port_a,
                       uint16_t port_b,
                       bool affinity_configured,
                       int cpu_a,
                       int cpu_b,
                       uint64_t line_b_delay_ns)
        : ip_(std::move(ip)),
          ports_{port_a, port_b},
          affinity_configured_(affinity_configured),
          cpu_ids_{cpu_a, cpu_b},
          line_b_delay_ns_(line_b_delay_ns) {
        if (affinity_configured_) {
            states_[0].affinity =
                astra::utils::pinCurrentThreadToCpu(cpu_ids_[0]);
            if (!states_[0].affinity) {
                throw std::runtime_error(
                    affinityError("line A", states_[0].affinity));
            }
        }
        sender_a_ = std::make_unique<UdpSender>(ip_.c_str(), ports_[0]);

        worker_b_ = std::thread(&RedundantUdpSender::lineBWorkerLoop, this);
        waitForStartup(states_[1]);
        if (states_[1].startup_status.load(std::memory_order_acquire) != 1) {
            const std::string error =
                "line B sender startup failed: " + states_[1].startup_error;
            stopAndJoin();
            throw std::runtime_error(error);
        }
    }

    ~RedundantUdpSender() { stopAndJoin(); }

    RedundantUdpSender(const RedundantUdpSender &) = delete;
    RedundantUdpSender &operator=(const RedundantUdpSender &) = delete;

    [[nodiscard]] bool
    send(const std::byte *data, std::size_t size, uint16_t msg_count) {
        if (worker_failed_.load(std::memory_order_acquire))
            throw std::runtime_error("line B sender failed: " + worker_error_);

        const bool line_a_sent = sender_a_->send(data, size);
        if (line_a_sent) {
            ++states_[0].packets_sent;
            states_[0].messages_sent += msg_count;
        } else {
            ++states_[0].send_failures;
        }
        handoff_.data = data;
        handoff_.size = size;
        handoff_.msg_count = msg_count;
        handoff_.line_b_send_succeeded = false;
        handoff_.line_b_not_before =
            std::chrono::steady_clock::now() +
            std::chrono::nanoseconds(
                static_cast<int64_t>(line_b_delay_ns_));

        const uint64_t generation =
            handoff_.generation.fetch_add(1, std::memory_order_release) + 1;

        uint64_t line_b_completed =
            handoff_.line_b_completed_generation.load(
                std::memory_order_acquire);
        while (line_b_completed != generation) {
            if (worker_failed_.load(std::memory_order_acquire)) {
                throw std::runtime_error("line B sender failed: " +
                                         worker_error_);
            }
            line_b_completed = handoff_.line_b_completed_generation.load(
                std::memory_order_acquire);
        }
        return line_a_sent && handoff_.line_b_send_succeeded;
    }

    [[nodiscard]] const RedundantLineState &line(std::size_t index) const {
        return states_[index];
    }

    [[nodiscard]] bool affinityConfigured() const {
        return affinity_configured_;
    }

private:
    static std::string affinityError(
        const char *line,
        const astra::utils::CpuAffinityResult &affinity) {
        return std::string(line) + " CPU affinity " +
               astra::utils::cpuAffinityStatusName(affinity.status) +
               " for CPU " + std::to_string(affinity.cpu_id) + ": " +
               affinity.message;
    }

    static void waitForStartup(RedundantLineState &state) {
        unsigned status =
            state.startup_status.load(std::memory_order_acquire);
        while (status == 0) {
            state.startup_status.wait(status, std::memory_order_acquire);
            status = state.startup_status.load(std::memory_order_acquire);
        }
    }

    void lineBWorkerLoop() noexcept {
        RedundantLineState &state = states_[1];
        bool startup_complete = false;
        try {
            if (affinity_configured_) {
                state.affinity =
                    astra::utils::pinCurrentThreadToCpu(cpu_ids_[1]);
                if (!state.affinity) {
                    throw std::runtime_error(
                        affinityError("line B", state.affinity));
                }
            }

            UdpSender sender(ip_.c_str(), ports_[1]);
            startup_complete = true;
            state.startup_status.store(1, std::memory_order_release);
            state.startup_status.notify_one();

            uint64_t observed_generation = 0;
            for (;;) {
                uint64_t generation =
                    handoff_.generation.load(std::memory_order_acquire);
                while (generation == observed_generation) {
                    generation =
                        handoff_.generation.load(std::memory_order_acquire);
                }
                observed_generation = generation;

                if (handoff_.stop.load(std::memory_order_acquire)) return;

                if (line_b_delay_ns_ > 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now > handoff_.line_b_not_before) {
                        ++state.delay_overruns;
                    }
                    while (g_running.load(std::memory_order_relaxed) &&
                           std::chrono::steady_clock::now() <
                               handoff_.line_b_not_before) {
                        // CPU B is dedicated to the redundant line. Busy
                        // polling avoids scheduler wakeup jitter at 1 us skew.
                    }
                }

                handoff_.line_b_send_succeeded =
                    sender.send(handoff_.data, handoff_.size);
                if (handoff_.line_b_send_succeeded) {
                    ++state.packets_sent;
                    state.messages_sent += handoff_.msg_count;
                } else {
                    ++state.send_failures;
                }

                handoff_.line_b_completed_generation.store(
                    generation, std::memory_order_release);
            }
        } catch (const std::exception &e) {
            if (!startup_complete) {
                state.startup_error = e.what();
                state.startup_status.store(2, std::memory_order_release);
                state.startup_status.notify_one();
            } else {
                worker_error_ = e.what();
                worker_failed_.store(true, std::memory_order_release);
            }
        } catch (...) {
            if (!startup_complete) {
                state.startup_error = "unknown exception";
                state.startup_status.store(2, std::memory_order_release);
                state.startup_status.notify_one();
            } else {
                worker_error_ = "unknown exception";
                worker_failed_.store(true, std::memory_order_release);
            }
        }
    }

    void stopAndJoin() noexcept {
        if (!handoff_.stop.exchange(true, std::memory_order_acq_rel)) {
            handoff_.generation.fetch_add(1, std::memory_order_release);
        }
        if (worker_b_.joinable()) worker_b_.join();
    }

    std::string ip_;
    std::array<uint16_t, 2> ports_{};
    bool affinity_configured_{false};
    std::array<int, 2> cpu_ids_{};
    uint64_t line_b_delay_ns_{0};
    RedundantPacketHandoff handoff_{};
    std::array<RedundantLineState, 2> states_{};
    std::unique_ptr<UdpSender> sender_a_;
    std::thread worker_b_{};
    std::atomic<bool> worker_failed_{false};
    std::string worker_error_;
};

} // namespace

int main(int argc, char *argv[]) {
    const bool redundant =
        argc >= 2 && std::string_view(argv[1]) == "--redundant";
    const bool valid_single_args = !redundant && argc >= 4 && argc <= 11;
    const bool valid_redundant_args = redundant && argc >= 9 && argc <= 13;
    if (!valid_single_args && !valid_redundant_args) {
        std::cerr << "Usage: " << argv[0]
                  << " <NASDAQ_ITCH50> <dest-ip> <port>"
                     " [msgs_per_packet] [session] [pkt/s] [premarket_seconds]"
                     " [ss_pause_seconds] [premarket_replay_mode]"
                     " [premarket_speedup]\n"
                  << "       " << argv[0]
                  << " --redundant <NASDAQ_ITCH50> <dest-ip> <port_a> <port_b>"
                     " <msgs_per_packet> <session> <pkt/s>"
                     " [premarket_seconds] [ss_pause_seconds]"
                     " [premarket_replay_mode] [premarket_speedup]\n"
                  << "  msgs_per_packet  ITCH messages bundled per UDP datagram"
                     " (default " << ItchMoldUdpSource::kDefaultMsgsPerPacket << ")\n"
                  << "  session          10-char MoldUDP64 session id"
                     " (default \"ASTRA     \")\n"
                  << "  pkt/s            packet upper rate limit;"
                     " 0 = max speed\n"
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
                     " (default ASTRA_PREMARKET_SPEEDUP or 1.0)\n"
                  << "  redundant CPU affinity: set both ASTRA_CPU_A and"
                     " ASTRA_CPU_B to distinct CPUs\n"
                  << "  redundant line skew: ASTRA_LINE_B_DELAY_NS"
                     " (default 1000; 0 disables)\n"
                  << "  optional receiver-readiness preamble:"
                     " ASTRA_STARTUP_HEARTBEAT_COUNT"
                     " (default 0) and"
                     " ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS"
                     " (default 1000; count 0 disables)\n"
                  << "  idle Mold traffic: ASTRA_HEARTBEAT_INTERVAL_MS"
                     " (default 1000; 0 disables)\n"
                  << "  end-of-session announcement:"
                     " ASTRA_EOS_PACKET_COUNT (default 10) and"
                     " ASTRA_EOS_INTERVAL_MS (default 100)\n"
                  << "  BinaryFILE completion:"
                     " ASTRA_BINARYFILE_COMPLETION=strict|legacy-sc-eof"
                     " (default strict)\n";
        return EXIT_FAILURE;
    }

    const int path_arg = redundant ? 2 : 1;
    const int ip_arg = redundant ? 3 : 2;
    const int port_a_arg = redundant ? 4 : 3;
    const int port_b_arg = redundant ? 5 : 0;
    const int msgs_per_packet_arg = redundant ? 6 : 4;
    const int session_arg = redundant ? 7 : 5;
    const int rate_arg = redundant ? 8 : 6;
    const int premarket_seconds_arg = redundant ? 9 : 7;
    const int ss_pause_seconds_arg = redundant ? 10 : 8;
    const int replay_mode_arg = redundant ? 11 : 9;
    const int speedup_arg = redundant ? 12 : 10;

    const std::string path = argv[path_arg];
    const char *ip = argv[ip_arg];
    uint16_t port_a = 0;
    if (!parsePort(argv[port_a_arg], port_a)) {
        std::cerr << "port must be an integer from 1 through "
                  << std::numeric_limits<uint16_t>::max() << "\n";
        return EXIT_FAILURE;
    }
    uint16_t port_b = 0;
    if (redundant && !parsePort(argv[port_b_arg], port_b)) {
        std::cerr << "port_b must be an integer from 1 through "
                  << std::numeric_limits<uint16_t>::max() << "\n";
        return EXIT_FAILURE;
    }
    if (redundant && port_a == port_b) {
        std::cerr << "port_a and port_b must be distinct when redundant mode"
                     " shares one destination IP\n";
        return EXIT_FAILURE;
    }
    uint16_t msgs_per_packet = ItchMoldUdpSource::kDefaultMsgsPerPacket;
    if (argc > msgs_per_packet_arg &&
        !parseMsgsPerPacket(argv[msgs_per_packet_arg], msgs_per_packet)) {
        std::cerr << "msgs_per_packet must be an integer from 1 through "
                  << std::numeric_limits<uint16_t>::max() << "\n";
        return EXIT_FAILURE;
    }
    const std::string session =
        argc > session_arg ? argv[session_arg] : "ASTRA     ";
    if (session.size() > MoldUdpPacketHeader::kSessionSize) {
        std::cerr << "session must not exceed "
                  << MoldUdpPacketHeader::kSessionSize << " bytes\n";
        return EXIT_FAILURE;
    }
    uint64_t pkts_per_second = 0;
    if (argc > rate_arg &&
        !parseU64InRange(argv[rate_arg], 0, kMaxPacketsPerSecond,
                         pkts_per_second)) {
        std::cerr << "pkt/s must be an integer from 0 through "
                  << kMaxPacketsPerSecond << "\n";
        return EXIT_FAILURE;
    }
    uint64_t premarket_seconds = 0;
    const char *premarket_seconds_value =
        argc > premarket_seconds_arg
            ? argv[premarket_seconds_arg]
            : std::getenv("ASTRA_PREMARKET_SECONDS");
    if (premarket_seconds_value != nullptr &&
        !parseU64InRange(premarket_seconds_value, 0,
                         kMaxSchedulingSeconds, premarket_seconds)) {
        std::cerr << "premarket_seconds must be an integer from 0 through "
                  << kMaxSchedulingSeconds << "\n";
        return EXIT_FAILURE;
    }
    uint64_t ss_pause_seconds = 0;
    const char *ss_pause_seconds_value =
        argc > ss_pause_seconds_arg
            ? argv[ss_pause_seconds_arg]
            : std::getenv("ASTRA_SS_PAUSE_SECONDS");
    if (ss_pause_seconds_value != nullptr &&
        !parseU64InRange(ss_pause_seconds_value, 0,
                         kMaxSchedulingSeconds, ss_pause_seconds)) {
        std::cerr << "ss_pause_seconds must be an integer from 0 through "
                  << kMaxSchedulingSeconds << "\n";
        return EXIT_FAILURE;
    }
    PremarketReplayMode replay_mode = PremarketReplayMode::Off;
    if (!parseReplayMode(argc > replay_mode_arg
                             ? argv[replay_mode_arg]
                             : std::getenv("ASTRA_PREMARKET_REPLAY_MODE"),
                         premarket_seconds, replay_mode)) {
        std::cerr << "premarket_replay_mode must be flat, timestamp, off,"
                     " none, or disabled\n";
        return EXIT_FAILURE;
    }
    double premarket_speedup = 1.0;
    const char *premarket_speedup_value =
        argc > speedup_arg ? argv[speedup_arg]
                           : std::getenv("ASTRA_PREMARKET_SPEEDUP");
    if (premarket_speedup_value != nullptr &&
        !parsePositiveDoubleStrict(premarket_speedup_value,
                                   premarket_speedup)) {
        std::cerr << "premarket_speedup must be a finite number greater"
                     " than zero\n";
        return EXIT_FAILURE;
    }

    bool affinity_configured = false;
    int cpu_a = -1;
    int cpu_b = -1;
    uint64_t line_b_delay_ns = kDefaultLineBDelayNs;
    uint64_t startup_heartbeat_count = kDefaultStartupHeartbeatCount;
    uint64_t startup_heartbeat_interval_ms =
        kDefaultStartupHeartbeatIntervalMs;
    uint64_t idle_heartbeat_interval_ms =
        kDefaultIdleHeartbeatIntervalMs;
    uint64_t end_of_session_packet_count =
        kDefaultEndOfSessionPacketCount;
    uint64_t end_of_session_interval_ms =
        kDefaultEndOfSessionIntervalMs;
    auto binary_file_completion_policy =
        ItchMoldUdpSource::CompletionPolicy::RequireTerminator;
    if (redundant) {
        const char *cpu_a_env = std::getenv("ASTRA_CPU_A");
        const char *cpu_b_env = std::getenv("ASTRA_CPU_B");
        const bool cpu_a_configured =
            cpu_a_env != nullptr && cpu_a_env[0] != '\0';
        const bool cpu_b_configured =
            cpu_b_env != nullptr && cpu_b_env[0] != '\0';
        if (cpu_a_configured != cpu_b_configured) {
            std::cerr << "ASTRA_CPU_A and ASTRA_CPU_B must be configured"
                         " together for redundant mode\n";
            return EXIT_FAILURE;
        }
        if (cpu_a_configured) {
            if (!parseCpuId(cpu_a_env, cpu_a) ||
                !parseCpuId(cpu_b_env, cpu_b)) {
                std::cerr << "ASTRA_CPU_A and ASTRA_CPU_B must be"
                             " non-negative integer CPU IDs\n";
                return EXIT_FAILURE;
            }
            if (cpu_a == cpu_b) {
                std::cerr << "ASTRA_CPU_A and ASTRA_CPU_B must identify"
                             " distinct CPUs\n";
                return EXIT_FAILURE;
            }
            affinity_configured = true;
        }

        if (const char *delay_env = std::getenv("ASTRA_LINE_B_DELAY_NS")) {
            if (!parseU64Strict(delay_env, line_b_delay_ns) ||
                line_b_delay_ns > kMaxLineBDelayNs) {
                std::cerr << "ASTRA_LINE_B_DELAY_NS must be an integer from"
                             " 0 through 1000000000\n";
                return EXIT_FAILURE;
            }
        }
    }

    if (const char *count_env =
            std::getenv("ASTRA_STARTUP_HEARTBEAT_COUNT")) {
        if (!parseU64Strict(count_env, startup_heartbeat_count) ||
            startup_heartbeat_count > kMaxStartupHeartbeatCount) {
            std::cerr << "ASTRA_STARTUP_HEARTBEAT_COUNT must be an integer"
                         " from 0 through "
                      << kMaxStartupHeartbeatCount << "\n";
            return EXIT_FAILURE;
        }
    }
    if (const char *interval_env =
            std::getenv("ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS")) {
        if (!parseU64Strict(interval_env, startup_heartbeat_interval_ms) ||
            startup_heartbeat_interval_ms >
                kMaxStartupHeartbeatIntervalMs) {
            std::cerr
                << "ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS must be an integer"
                   " from 0 through "
                << kMaxStartupHeartbeatIntervalMs << "\n";
            return EXIT_FAILURE;
        }
    }
    if (const char *interval_env =
            std::getenv("ASTRA_HEARTBEAT_INTERVAL_MS")) {
        if (!parseU64Strict(interval_env, idle_heartbeat_interval_ms) ||
            idle_heartbeat_interval_ms >
                kMaxIdleHeartbeatIntervalMs) {
            std::cerr << "ASTRA_HEARTBEAT_INTERVAL_MS must be an integer"
                         " from 0 through "
                      << kMaxIdleHeartbeatIntervalMs << "\n";
            return EXIT_FAILURE;
        }
    }
    if (const char *count_env =
            std::getenv("ASTRA_EOS_PACKET_COUNT")) {
        if (!parseU64Strict(count_env, end_of_session_packet_count) ||
            end_of_session_packet_count == 0 ||
            end_of_session_packet_count >
                kMaxEndOfSessionPacketCount) {
            std::cerr << "ASTRA_EOS_PACKET_COUNT must be an integer"
                         " from 1 through "
                      << kMaxEndOfSessionPacketCount << "\n";
            return EXIT_FAILURE;
        }
    }
    if (const char *interval_env =
            std::getenv("ASTRA_EOS_INTERVAL_MS")) {
        if (!parseU64Strict(interval_env, end_of_session_interval_ms) ||
            end_of_session_interval_ms >
                kMaxEndOfSessionIntervalMs) {
            std::cerr << "ASTRA_EOS_INTERVAL_MS must be an integer"
                         " from 0 through "
                      << kMaxEndOfSessionIntervalMs << "\n";
            return EXIT_FAILURE;
        }
    }
    if (const char *completion_env =
            std::getenv("ASTRA_BINARYFILE_COMPLETION")) {
        const std::string_view value(completion_env);
        if (value == "strict") {
            binary_file_completion_policy =
                ItchMoldUdpSource::CompletionPolicy::RequireTerminator;
        } else if (value == "legacy-sc-eof") {
            binary_file_completion_policy =
                ItchMoldUdpSource::CompletionPolicy::
                    AllowEofAfterEndOfMessages;
        } else {
            std::cerr << "ASTRA_BINARYFILE_COMPLETION must be"
                         " strict or legacy-sc-eof\n";
            return EXIT_FAILURE;
        }
    }

    try {
        if (!redundant) {
            const char *cpu_env = std::getenv("ASTRA_CPU");
            if (cpu_env != nullptr && cpu_env[0] != '\0') {
                int cpu_id = -1;
                if (!parseCpuId(cpu_env, cpu_id)) {
                    throw std::invalid_argument(
                        "ASTRA_CPU must be a non-negative in-range integer"
                        " CPU ID");
                }
                const auto affinity =
                    astra::utils::pinCurrentThreadToCpu(cpu_id);
                std::cout
                    << "cpu_affinity status="
                    << astra::utils::cpuAffinityStatusName(affinity.status)
                    << " cpu=" << affinity.cpu_id;
                if (!affinity)
                    std::cout << " error=" << affinity.error_code
                              << " message=\"" << affinity.message << "\"";
                std::cout << "\n";
                if (!affinity) {
                    throw std::runtime_error(
                        "requested CPU affinity is required for this run");
                }
            }
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

        ItchMoldUdpSource source(path, session, msgs_per_packet,
                                 binary_file_completion_policy);
        if (!source.isOpen()) {
            std::cerr << source.lastError() << "\n";
            return EXIT_FAILURE;
        }

        std::signal(SIGINT,  onSignal);
        std::signal(SIGTERM, onSignal);

        std::unique_ptr<UdpSender> sender_a;
        std::unique_ptr<RedundantUdpSender> redundant_sender;
        if (redundant) {
            redundant_sender = std::make_unique<RedundantUdpSender>(
                ip, port_a, port_b, affinity_configured, cpu_a, cpu_b,
                line_b_delay_ns);
            if (redundant_sender->affinityConfigured()) {
                for (std::size_t line = 0; line < 2; ++line) {
                    const auto &affinity =
                        redundant_sender->line(line).affinity;
                    std::cout << "cpu_affinity line="
                              << (line == 0 ? "A" : "B")
                              << " status="
                              << astra::utils::cpuAffinityStatusName(
                                     affinity.status)
                              << " cpu=" << affinity.cpu_id << "\n";
                }
            }
        } else {
            sender_a = std::make_unique<UdpSender>(ip, port_a);
        }

        std::cout << "MoldUDP64/ITCH5.0 sender";
        if (redundant) {
            std::cout << " redundant -> line_a=" << ip << ":" << port_a
                      << " line_b=" << ip << ":" << port_b
                      << " line_b_delay_ns=" << line_b_delay_ns
                      << " send_threads=2";
        } else {
            std::cout << " -> " << ip << ":" << port_a;
        }
        std::cout << "  file=" << path
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
        std::cout << "  startup_heartbeats=" << startup_heartbeat_count
                  << " startup_heartbeat_interval_ms="
                  << startup_heartbeat_interval_ms
                  << " heartbeat_interval_ms="
                  << idle_heartbeat_interval_ms
                  << " eos_packet_count="
                  << end_of_session_packet_count
                  << " eos_interval_ms="
                  << end_of_session_interval_ms
                  << " binaryfile_completion="
                  << (binary_file_completion_policy ==
                              ItchMoldUdpSource::CompletionPolicy::
                                  RequireTerminator
                          ? "strict"
                          : "legacy-sc-eof");
        std::cout << "\n  (press Ctrl+C to stop)\n";
        std::cout << std::flush;

        astra::protocol::MoldUdpPacketRateSchedule packet_rate_schedule(
            pkts_per_second);
        auto next_premarket_send = std::chrono::steady_clock::now();
        auto premarket_start = std::chrono::steady_clock::now();
        bool premarket_active = false;
        bool ss_pause_pending = ss_pause_seconds > 0;

        uint64_t line_a_pkts_sent = 0;
        uint64_t line_a_msgs_sent = 0;
        uint64_t line_a_send_failures = 0;
        uint64_t line_b_pkts_sent = 0;
        uint64_t line_b_msgs_sent = 0;
        uint64_t line_b_send_failures = 0;
        uint64_t line_b_delay_overruns = 0;
        uint64_t logical_packets = 0;
        uint64_t logical_messages = 0;
        uint64_t logical_first_seq = 0;
        uint64_t logical_next_seq = 0;
        uint64_t startup_heartbeats_sent = 0;
        uint64_t periodic_heartbeats_sent = 0;
        uint64_t end_of_session_packets_sent = 0;
        bool transmission_failed = false;
        PacketView packet;

        astra::protocol::MoldUdpHeartbeatSchedule heartbeat_schedule{
            std::chrono::milliseconds(idle_heartbeat_interval_ms)};
        const astra::protocol::MoldUdpEndOfSessionSchedule
            end_of_session_schedule(
                end_of_session_packet_count,
                std::chrono::milliseconds(end_of_session_interval_ms));

        const auto send_control_packet =
            [&](const astra::protocol::MoldUdpControlPacket &control_packet,
                uint64_t next_sequence) {
                bool sent = false;
                if (redundant_sender != nullptr) {
                    sent = redundant_sender->send(control_packet.data(),
                                                  control_packet.size(), 0);
                } else {
                    sent = sender_a->send(control_packet.data(),
                                          control_packet.size());
                    if (sent)
                        ++line_a_pkts_sent;
                    else
                        ++line_a_send_failures;
                }
                if (!sent) {
                    transmission_failed = true;
                    return false;
                }
                ++logical_packets;
                logical_next_seq = next_sequence;
                return true;
            };

        const auto wait_until_with_heartbeats =
            [&](std::chrono::steady_clock::time_point deadline,
                uint64_t next_sequence) {
                while (g_running.load(std::memory_order_relaxed)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) return true;

                    const auto heartbeat_deadline =
                        heartbeat_schedule.nextHeartbeatBefore(deadline);
                    if (!heartbeat_deadline.has_value())
                        return sleepUntilOrStopped(deadline);

                    if (!sleepUntilOrStopped(*heartbeat_deadline))
                        return false;

                    const auto heartbeat =
                        astra::protocol::makeMoldHeartbeatPacket(
                            session, next_sequence);
                    if (!send_control_packet(heartbeat, next_sequence))
                        return false;
                    ++periodic_heartbeats_sent;
                    heartbeat_schedule.observeTraffic(
                        std::chrono::steady_clock::now());
                }
                return false;
            };

        if (startup_heartbeat_count != 0) {
            const auto startup_heartbeat =
                astra::protocol::makeMoldHeartbeatPacket(
                    session, source.nextSequence());
            auto heartbeat_deadline = std::chrono::steady_clock::now();
            const auto heartbeat_interval = std::chrono::milliseconds(
                startup_heartbeat_interval_ms);

            for (uint64_t index = 0;
                 index < startup_heartbeat_count &&
                 g_running.load(std::memory_order_relaxed);
                 ++index) {
                if (!send_control_packet(startup_heartbeat,
                                         source.nextSequence()))
                    break;
                ++startup_heartbeats_sent;

                if (heartbeat_interval.count() > 0) {
                    heartbeat_deadline = checkedDeadlineAfter(
                        heartbeat_deadline, heartbeat_interval,
                        "startup heartbeat deadline");
                    if (!sleepUntilOrStopped(heartbeat_deadline))
                        break;
                }
            }
        }
        if (heartbeat_schedule.enabled() && !transmission_failed) {
            heartbeat_schedule.observeTraffic(
                std::chrono::steady_clock::now());
        }
        packet_rate_schedule.reset(std::chrono::steady_clock::now());

        while (g_running.load(std::memory_order_relaxed) &&
               !transmission_failed) {
            if (!source.next(packet)) break;

            const uint64_t first_seq = readU64BE(packet.data + 10);
            const uint16_t msg_count = readU16BE(packet.data + 18);
            const bool inspect_timing = premarket_enabled || ss_pause_pending;
            const PacketTiming packet_timing =
                inspect_timing ? inspectPacket(packet) : PacketTiming{};
            const bool contains_ss = packet_timing.contains_ss;
            const bool contains_sq = packet_timing.contains_sq;

            if (premarket_active) {
                if (replay_mode == PremarketReplayMode::Flat) {
                    if (!wait_until_with_heartbeats(next_premarket_send,
                                                    first_seq)) {
                        break;
                    }
                    next_premarket_send = checkedDeadlineAfter(
                        next_premarket_send, premarket_interval,
                        "flat premarket deadline");
                } else if (replay_mode == PremarketReplayMode::Timestamp) {
                    uint64_t packet_timestamp = packet_timing.first_timestamp_ns;
                    if (packet_timing.contains_sq)
                        packet_timestamp = packet_timing.sq_timestamp_ns;
                    if (packet_timing.has_timestamp &&
                        packet_timestamp > premarket.ss_timestamp_ns) {
                        const auto packet_deadline = checkedDeadlineAfter(
                            premarket_start,
                            scaledDelay(packet_timestamp -
                                            premarket.ss_timestamp_ns,
                                        premarket_speedup),
                            "timestamp premarket deadline");
                        if (!wait_until_with_heartbeats(packet_deadline,
                                                        first_seq)) {
                            break;
                        }
                    }
                }
            }

            bool packet_sent = false;
            if (redundant_sender != nullptr) {
                // PacketView aliases the source's reusable packet buffer.
                // This call returns only after A and B have completed sendto,
                // so source.next() cannot mutate bytes still used by a line.
                packet_sent =
                    redundant_sender->send(packet.data, packet.size,
                                           msg_count);
            } else {
                packet_sent = sender_a->send(packet.data, packet.size);
                if (packet_sent) {
                    ++line_a_pkts_sent;
                    line_a_msgs_sent += msg_count;
                } else {
                    ++line_a_send_failures;
                }
            }
            if (!packet_sent) {
                transmission_failed = true;
                break;
            }

            ++logical_packets;
            logical_messages += msg_count;
            if (logical_first_seq == 0)
                logical_first_seq = first_seq;
            logical_next_seq = first_seq + msg_count;

            const auto data_sent_at = std::chrono::steady_clock::now();
            if (heartbeat_schedule.enabled()) {
                heartbeat_schedule.observeTraffic(data_sent_at);
            }

            std::optional<std::chrono::steady_clock::time_point>
                next_ordinary_send;
            if (packet_rate_schedule.enabled() &&
                (!premarket_active || contains_sq)) {
                if (contains_sq)
                    packet_rate_schedule.reset(data_sent_at);
                next_ordinary_send =
                    packet_rate_schedule.checkedDeadlineAfterPacket(
                        data_sent_at);
                if (!next_ordinary_send.has_value()) {
                    throw std::out_of_range(
                        "packet-rate deadline exceeds steady-clock range");
                }
            }

            if (contains_ss) {
                if (ss_pause_pending) {
                    const auto pause_deadline = checkedDeadlineAfter(
                        std::chrono::steady_clock::now(),
                        std::chrono::seconds(ss_pause_seconds),
                        "System Event S pause deadline");
                    const bool pause_completed = wait_until_with_heartbeats(
                        pause_deadline, source.nextSequence());
                    ss_pause_pending = false;
                    if (!pause_completed) break;
                }
                if (premarket_enabled && !premarket_active) {
                    premarket_active = true;
                    premarket_start = std::chrono::steady_clock::now();
                    next_premarket_send = checkedDeadlineAfter(
                        premarket_start, premarket_interval,
                        "initial premarket deadline");
                    if (replay_mode == PremarketReplayMode::Timestamp &&
                        !source.setMessagesPerPacket(1)) {
                        throw std::logic_error(
                            "failed to enable timestamp packetization");
                    }
                }
            }

            if (contains_sq) {
                premarket_active = false;
                if (replay_mode == PremarketReplayMode::Timestamp &&
                    !source.setMessagesPerPacket(msgs_per_packet)) {
                    throw std::logic_error(
                        "failed to restore configured packetization");
                }
            }

            if (!premarket_active && next_ordinary_send.has_value() &&
                !source.completed() && source.lastError().empty() &&
                !transmission_failed) {
                if (!wait_until_with_heartbeats(*next_ordinary_send,
                                                source.nextSequence())) {
                    break;
                }
            }
        }

        bool end_of_session_sent = false;
        if (source.completed() && !transmission_failed) {
            const uint64_t end_sequence = source.nextSequence();
            const auto end_packet =
                astra::protocol::makeMoldEndOfSessionPacket(
                    session, end_sequence);

            std::optional<std::chrono::steady_clock::time_point>
                previous_end_of_session_send;
            for (uint64_t index = 0;
                 index < end_of_session_schedule.packetCount() &&
                 g_running.load(std::memory_order_relaxed);
                 ++index) {
                if (previous_end_of_session_send.has_value()) {
                    // Rebase every copy on the preceding successful send. A
                    // delayed wakeup must not compress later EOS copies into
                    // a catch-up burst.
                    const auto deadline =
                        end_of_session_schedule.checkedDeadlineAfterSend(
                            *previous_end_of_session_send);
                    if (!deadline.has_value()) {
                        throw std::out_of_range(
                            "end-of-session deadline exceeds "
                            "steady-clock range");
                    }
                    if (!sleepUntilOrStopped(*deadline))
                        break;
                }
                if (!send_control_packet(end_packet, end_sequence))
                    break;
                previous_end_of_session_send =
                    std::chrono::steady_clock::now();
                if (index == 0) {
                    if (logical_first_seq == 0)
                        logical_first_seq = end_sequence;
                }
                ++end_of_session_packets_sent;
            }
            end_of_session_sent =
                end_of_session_packets_sent ==
                end_of_session_schedule.packetCount();
        }

        const bool interrupted =
            !g_running.load(std::memory_order_relaxed);
        const char *completion = "source_error";
        if (transmission_failed)
            completion = "send_error";
        else if (source.completed() && end_of_session_sent)
            completion = "complete";
        else if (interrupted)
            completion = "interrupted";
        else if (source.completed())
            completion = "eos_incomplete";
        if (!source.lastError().empty()) {
            std::cerr << "Replay source error: " << source.lastError() << "\n";
        }

        if (redundant) {
            const RedundantLineState &line_a = redundant_sender->line(0);
            const RedundantLineState &line_b = redundant_sender->line(1);
            line_a_pkts_sent = line_a.packets_sent;
            line_a_msgs_sent = line_a.messages_sent;
            line_a_send_failures = line_a.send_failures;
            line_b_pkts_sent = line_b.packets_sent;
            line_b_msgs_sent = line_b.messages_sent;
            line_b_send_failures = line_b.send_failures;
            line_b_delay_overruns = line_b.delay_overruns;
            std::cout << "sender_stats completion=" << completion
                      << " line_a_pkts_sent=" << line_a_pkts_sent
                      << " line_a_msgs_sent=" << line_a_msgs_sent
                      << " line_a_send_failures=" << line_a_send_failures
                      << " line_b_pkts_sent=" << line_b_pkts_sent
                      << " line_b_msgs_sent=" << line_b_msgs_sent
                      << " line_b_send_failures=" << line_b_send_failures
                      << " line_b_delay_ns=" << line_b_delay_ns
                      << " line_b_delay_overruns="
                      << line_b_delay_overruns
                      << " logical_packets=" << logical_packets
                      << " logical_messages=" << logical_messages
                      << " startup_heartbeats_sent="
                      << startup_heartbeats_sent
                      << " periodic_heartbeats_sent="
                      << periodic_heartbeats_sent
                      << " eos_packets_sent="
                      << end_of_session_packets_sent
                      << " eos_packets_expected="
                      << end_of_session_schedule.packetCount()
                      << " first_seq=" << logical_first_seq
                      << " next_seq=" << logical_next_seq
                      << " binaryfile_completion="
                      << completionKindName(source.completionKind())
                      << " end_of_session_sent="
                      << (end_of_session_sent ? "true" : "false") << "\n";
        } else {
            std::cout << "sender_stats completion=" << completion
                      << " pkts_sent=" << line_a_pkts_sent
                      << " msgs_sent=" << line_a_msgs_sent
                      << " startup_heartbeats_sent="
                      << startup_heartbeats_sent
                      << " periodic_heartbeats_sent="
                      << periodic_heartbeats_sent
                      << " eos_packets_sent="
                      << end_of_session_packets_sent
                      << " eos_packets_expected="
                      << end_of_session_schedule.packetCount()
                      << " first_seq=" << logical_first_seq
                      << " next_seq=" << logical_next_seq
                      << " send_failures=" << line_a_send_failures
                      << " binaryfile_completion="
                      << completionKindName(source.completionKind())
                      << " end_of_session_sent="
                      << (end_of_session_sent ? "true" : "false") << "\n";
        }
        const bool redundant_counts_clean =
            !redundant ||
            (line_a_pkts_sent == logical_packets &&
             line_b_pkts_sent == logical_packets &&
             line_a_msgs_sent == logical_messages &&
             line_b_msgs_sent == logical_messages);
        return source.completed() && !transmission_failed &&
                       end_of_session_sent &&
                       line_a_send_failures == 0 &&
                       line_b_send_failures == 0 && redundant_counts_clean
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    } catch (const std::exception &e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
