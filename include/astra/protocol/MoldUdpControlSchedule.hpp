#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace astra::protocol {

// Enforces a strict upper rate for sequenced MoldUDP64 packets. The interval is
// rounded up to the next representable steady-clock tick, so the schedule never
// asks the sender to exceed the configured packets-per-second rate. A late send
// rebases the next deadline instead of accumulating a catch-up burst.
class MoldUdpPacketRateSchedule {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;
  using TimePoint = Clock::time_point;

  static constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000;
  static constexpr uint64_t kMaximumPacketsPerSecond =
      kNanosecondsPerSecond;

  explicit MoldUdpPacketRateSchedule(uint64_t packets_per_second) noexcept
      : interval_(intervalFor(packets_per_second)) {}

  [[nodiscard]] bool enabled() const noexcept {
    return interval_ > Duration::zero();
  }

  [[nodiscard]] Duration interval() const noexcept { return interval_; }

  void reset(TimePoint base) noexcept { scheduled_deadline_ = base; }

  [[nodiscard]] std::optional<TimePoint>
  checkedDeadlineAfterPacket(TimePoint sent_at) noexcept {
    if (!enabled())
      return std::nullopt;

    const TimePoint base =
        scheduled_deadline_.has_value() && *scheduled_deadline_ > sent_at
            ? *scheduled_deadline_
            : sent_at;
    const std::optional<TimePoint> candidate = checkedAdd(base);
    if (!candidate.has_value())
      return std::nullopt;

    scheduled_deadline_ = *candidate;
    return candidate;
  }

private:
  static Duration intervalFor(uint64_t packets_per_second) noexcept {
    if (packets_per_second == 0)
      return Duration::zero();

    const uint64_t whole_nanoseconds =
        kNanosecondsPerSecond / packets_per_second;
    const uint64_t remainder =
        kNanosecondsPerSecond % packets_per_second;
    const uint64_t ceiling_nanoseconds =
        whole_nanoseconds + (remainder == 0 ? 0u : 1u);
    return std::chrono::ceil<Duration>(
        std::chrono::nanoseconds(ceiling_nanoseconds));
  }

  [[nodiscard]] std::optional<TimePoint>
  checkedAdd(TimePoint base) const noexcept {
    if (base > TimePoint::max() - interval_)
      return std::nullopt;
    return base + interval_;
  }

  Duration                 interval_{};
  std::optional<TimePoint> scheduled_deadline_{};
};

// Tracks when the sender must emit a MoldUDP64 heartbeat while no sequenced
// data is being transmitted. Any data/control transmission resets the idle
// interval; heartbeat deadlines never accumulate into a catch-up burst after
// a delayed wakeup.
class MoldUdpHeartbeatSchedule {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;
  using TimePoint = Clock::time_point;

  explicit MoldUdpHeartbeatSchedule(Duration interval) noexcept
      : interval_(interval) {}

  [[nodiscard]] bool enabled() const noexcept {
    return interval_ > Duration::zero();
  }

  [[nodiscard]] Duration interval() const noexcept { return interval_; }

  void observeTraffic(TimePoint sent_at) noexcept {
    if (!enabled()) {
      next_heartbeat_.reset();
      return;
    }
    next_heartbeat_ =
        sent_at > TimePoint::max() - interval_
            ? TimePoint::max()
            : sent_at + interval_;
  }

  // A sequenced packet takes precedence when its deadline is exactly equal to
  // the heartbeat deadline because it already supplies Mold traffic.
  [[nodiscard]] std::optional<TimePoint>
  nextHeartbeatBefore(TimePoint data_deadline) const noexcept {
    if (!next_heartbeat_.has_value() ||
        *next_heartbeat_ >= data_deadline) {
      return std::nullopt;
    }
    return next_heartbeat_;
  }

  [[nodiscard]] std::optional<TimePoint> nextHeartbeat() const noexcept {
    return next_heartbeat_;
  }

private:
  Duration                 interval_{};
  std::optional<TimePoint> next_heartbeat_{};
};

// Defines the short MoldUDP64 end-of-session announcement window. The count
// is the total number of EOS packets, including the first packet.
class MoldUdpEndOfSessionSchedule {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;
  using TimePoint = Clock::time_point;

  MoldUdpEndOfSessionSchedule(uint64_t packet_count,
                              Duration interval) noexcept
      : packet_count_(packet_count), interval_(interval) {}

  [[nodiscard]] uint64_t packetCount() const noexcept {
    return packet_count_;
  }

  [[nodiscard]] Duration interval() const noexcept { return interval_; }

  // Schedule the next copy relative to the preceding successful send. Callers
  // use this form for the live EOS burst so a delayed copy cannot cause later
  // copies to catch up back-to-back.
  [[nodiscard]] std::optional<TimePoint>
  checkedDeadlineAfterSend(TimePoint successful_send) const noexcept {
    return checkedDeadline(successful_send, 1);
  }

  [[nodiscard]] TimePoint deadline(TimePoint first_send,
                                   uint64_t packet_index) const noexcept {
    return checkedDeadline(first_send, packet_index)
        .value_or(TimePoint::max());
  }

  [[nodiscard]] std::optional<TimePoint>
  checkedDeadline(TimePoint first_send,
                  uint64_t packet_index) const noexcept {
    if (packet_index == 0 || interval_ <= Duration::zero())
      return first_send;

    using Rep = Duration::rep;
    const auto interval_count = interval_.count();
    if (interval_count <= 0)
      return std::nullopt;
    const std::uint64_t maximum_index =
        static_cast<std::uint64_t>(
            Duration::max().count() / interval_count);
    if (packet_index > maximum_index)
      return std::nullopt;
    const Duration delay =
        interval_ * static_cast<Rep>(packet_index);
    if (first_send > TimePoint::max() - delay)
      return std::nullopt;
    return first_send + delay;
  }

private:
  uint64_t packet_count_{0};
  Duration interval_{};
};

} // namespace astra::protocol
