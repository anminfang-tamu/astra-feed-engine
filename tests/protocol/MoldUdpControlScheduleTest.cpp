#include "astra/protocol/MoldUdpControlSchedule.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using PacketRateSchedule = astra::protocol::MoldUdpPacketRateSchedule;
using HeartbeatSchedule = astra::protocol::MoldUdpHeartbeatSchedule;
using EndOfSessionSchedule = astra::protocol::MoldUdpEndOfSessionSchedule;

TEST(MoldUdpControlScheduleTest,
     PacketRateRoundsUpToRemainAConfigurableUpperBound) {
  const PacketRateSchedule documented_non_divisor(150'000);
  EXPECT_TRUE(documented_non_divisor.enabled());
  EXPECT_EQ(documented_non_divisor.interval(),
            std::chrono::nanoseconds(6'667));

  const PacketRateSchedule high_non_divisor(500'000'001);
  EXPECT_EQ(high_non_divisor.interval(), std::chrono::nanoseconds(2));

  const PacketRateSchedule exact_divisor(100'000);
  EXPECT_EQ(exact_divisor.interval(), std::chrono::nanoseconds(10'000));
}

TEST(MoldUdpControlScheduleTest, PacketRateLateSendRebasesWithoutCatchUp) {
  PacketRateSchedule schedule(100'000);
  const auto start =
      PacketRateSchedule::TimePoint{} + std::chrono::seconds(10);
  schedule.reset(start);

  const auto first_deadline = schedule.checkedDeadlineAfterPacket(start);
  ASSERT_TRUE(first_deadline.has_value());
  EXPECT_EQ(*first_deadline, start + std::chrono::microseconds(10));

  const auto late_send = start + std::chrono::milliseconds(100);
  const auto rebased_deadline =
      schedule.checkedDeadlineAfterPacket(late_send);
  ASSERT_TRUE(rebased_deadline.has_value());
  EXPECT_EQ(*rebased_deadline, late_send + std::chrono::microseconds(10));
}

TEST(MoldUdpControlScheduleTest,
     PacketRateSlightlyLateSendStillRequiresOneFullInterval) {
  PacketRateSchedule schedule(2);
  const auto start =
      PacketRateSchedule::TimePoint{} + std::chrono::seconds(10);
  schedule.reset(start);

  const auto first_deadline = schedule.checkedDeadlineAfterPacket(start);
  ASSERT_TRUE(first_deadline.has_value());
  EXPECT_EQ(*first_deadline, start + std::chrono::milliseconds(500));

  const auto slightly_late_send =
      start + std::chrono::milliseconds(600);
  const auto rebased_deadline =
      schedule.checkedDeadlineAfterPacket(slightly_late_send);
  ASSERT_TRUE(rebased_deadline.has_value());
  EXPECT_EQ(*rebased_deadline,
            slightly_late_send + std::chrono::milliseconds(500));
}

TEST(MoldUdpControlScheduleTest,
     PacketRateResetAtModeBoundaryRequiresOneFullInterval) {
  PacketRateSchedule schedule(2);
  const auto boundary_send =
      PacketRateSchedule::TimePoint{} + std::chrono::seconds(10);
  schedule.reset(boundary_send);

  const auto next_deadline =
      schedule.checkedDeadlineAfterPacket(boundary_send);
  ASSERT_TRUE(next_deadline.has_value());
  EXPECT_EQ(*next_deadline, boundary_send + std::chrono::milliseconds(500));
}

TEST(MoldUdpControlScheduleTest, PacketRateZeroDisablesAndMaximumUsesOneTick) {
  PacketRateSchedule disabled(0);
  const auto now =
      PacketRateSchedule::TimePoint{} + std::chrono::seconds(10);
  disabled.reset(now);
  EXPECT_FALSE(disabled.enabled());
  EXPECT_EQ(disabled.interval(), PacketRateSchedule::Duration::zero());
  EXPECT_FALSE(disabled.checkedDeadlineAfterPacket(now).has_value());

  const PacketRateSchedule maximum(
      PacketRateSchedule::kMaximumPacketsPerSecond);
  EXPECT_TRUE(maximum.enabled());
  EXPECT_EQ(maximum.interval(), std::chrono::nanoseconds(1));
}

TEST(MoldUdpControlScheduleTest,
     PacketRateClockBoundaryAcceptsExactMaximumAndRejectsOverflow) {
  PacketRateSchedule schedule(100'000);
  const auto interval = std::chrono::microseconds(10);
  const auto last_safe = PacketRateSchedule::TimePoint::max() - interval;
  schedule.reset(last_safe);

  const auto maximum_deadline =
      schedule.checkedDeadlineAfterPacket(last_safe);
  ASSERT_TRUE(maximum_deadline.has_value());
  EXPECT_EQ(*maximum_deadline, PacketRateSchedule::TimePoint::max());

  EXPECT_FALSE(
      schedule
          .checkedDeadlineAfterPacket(PacketRateSchedule::TimePoint::max())
          .has_value());
}

TEST(MoldUdpControlScheduleTest, DisabledHeartbeatNeverSchedulesTraffic) {
  const HeartbeatSchedule schedule(HeartbeatSchedule::Duration::zero());
  const auto now = HeartbeatSchedule::TimePoint{} + std::chrono::seconds(10);

  EXPECT_FALSE(schedule.enabled());
  EXPECT_FALSE(schedule.nextHeartbeat().has_value());
  EXPECT_FALSE(schedule.nextHeartbeatBefore(now + std::chrono::hours(1))
                   .has_value());
}

TEST(MoldUdpControlScheduleTest, TrafficStartsAndResetsIdleInterval) {
  HeartbeatSchedule schedule(std::chrono::seconds(1));
  const auto start =
      HeartbeatSchedule::TimePoint{} + std::chrono::seconds(10);

  schedule.observeTraffic(start);
  ASSERT_TRUE(schedule.nextHeartbeat().has_value());
  EXPECT_EQ(*schedule.nextHeartbeat(), start + std::chrono::seconds(1));

  schedule.observeTraffic(start + std::chrono::milliseconds(750));
  ASSERT_TRUE(schedule.nextHeartbeat().has_value());
  EXPECT_EQ(*schedule.nextHeartbeat(),
            start + std::chrono::milliseconds(1750));
}

TEST(MoldUdpControlScheduleTest,
     SequencedPacketWinsWhenDueAtHeartbeatDeadline) {
  HeartbeatSchedule schedule(std::chrono::seconds(1));
  const auto start =
      HeartbeatSchedule::TimePoint{} + std::chrono::seconds(10);
  schedule.observeTraffic(start);

  EXPECT_FALSE(
      schedule.nextHeartbeatBefore(start + std::chrono::milliseconds(500))
          .has_value());
  EXPECT_FALSE(
      schedule.nextHeartbeatBefore(start + std::chrono::seconds(1))
          .has_value());

  const auto heartbeat =
      schedule.nextHeartbeatBefore(start + std::chrono::seconds(2));
  ASSERT_TRUE(heartbeat.has_value());
  EXPECT_EQ(*heartbeat, start + std::chrono::seconds(1));
}

TEST(MoldUdpControlScheduleTest, LateHeartbeatDoesNotCreateCatchUpBurst) {
  HeartbeatSchedule schedule(std::chrono::seconds(1));
  const auto start =
      HeartbeatSchedule::TimePoint{} + std::chrono::seconds(10);
  schedule.observeTraffic(start);

  const auto late_send = start + std::chrono::milliseconds(1400);
  schedule.observeTraffic(late_send);

  ASSERT_TRUE(schedule.nextHeartbeat().has_value());
  EXPECT_EQ(*schedule.nextHeartbeat(),
            start + std::chrono::milliseconds(2400));
}

TEST(MoldUdpControlScheduleTest, EndOfSessionWindowUsesTotalPacketCount) {
  const EndOfSessionSchedule schedule(10, std::chrono::milliseconds(100));
  const auto first_send =
      EndOfSessionSchedule::TimePoint{} + std::chrono::seconds(10);

  EXPECT_EQ(schedule.packetCount(), 10u);
  EXPECT_EQ(schedule.deadline(first_send, 0), first_send);
  EXPECT_EQ(schedule.deadline(first_send, 1),
            first_send + std::chrono::milliseconds(100));
  EXPECT_EQ(schedule.deadline(first_send, 9),
            first_send + std::chrono::milliseconds(900));
}

TEST(MoldUdpControlScheduleTest,
     EndOfSessionNextCopyRebasesAfterALateSuccessfulSend) {
  const EndOfSessionSchedule schedule(10, std::chrono::milliseconds(100));
  const auto first_send =
      EndOfSessionSchedule::TimePoint{} + std::chrono::seconds(10);

  const auto second_deadline =
      schedule.checkedDeadlineAfterSend(first_send);
  ASSERT_TRUE(second_deadline.has_value());
  EXPECT_EQ(*second_deadline, first_send + std::chrono::milliseconds(100));

  const auto late_second_send =
      first_send + std::chrono::milliseconds(250);
  const auto third_deadline =
      schedule.checkedDeadlineAfterSend(late_second_send);
  ASSERT_TRUE(third_deadline.has_value());
  EXPECT_EQ(*third_deadline,
            late_second_send + std::chrono::milliseconds(100));
}

TEST(MoldUdpControlScheduleTest, ClockBoundariesSaturateOrRejectWithoutOverflow) {
  HeartbeatSchedule heartbeat(std::chrono::seconds(1));
  const auto near_max =
      HeartbeatSchedule::TimePoint::max() - std::chrono::milliseconds(500);
  heartbeat.observeTraffic(near_max);
  ASSERT_TRUE(heartbeat.nextHeartbeat().has_value());
  EXPECT_EQ(*heartbeat.nextHeartbeat(), HeartbeatSchedule::TimePoint::max());

  const auto minimum = HeartbeatSchedule::TimePoint::min();
  heartbeat.observeTraffic(minimum);
  ASSERT_TRUE(heartbeat.nextHeartbeat().has_value());
  EXPECT_EQ(*heartbeat.nextHeartbeat(), minimum + std::chrono::seconds(1));

  const EndOfSessionSchedule ordinary(2, std::chrono::seconds(1));
  EXPECT_FALSE(ordinary.checkedDeadline(near_max, 1).has_value());
  EXPECT_EQ(ordinary.deadline(near_max, 1),
            EndOfSessionSchedule::TimePoint::max());

  const EndOfSessionSchedule multiplication_overflow(
      3, EndOfSessionSchedule::Duration::max());
  EXPECT_FALSE(
      multiplication_overflow.checkedDeadline(
          EndOfSessionSchedule::TimePoint{}, 2)
          .has_value());
}

} // namespace
