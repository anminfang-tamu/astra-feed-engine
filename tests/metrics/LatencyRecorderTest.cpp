#include "astra/metrics/LatencyRecorder.hpp"

#include <gtest/gtest.h>

#include <sstream>

TEST(LatencyRecorderTest, RecordsBasicStats) {
  LatencyRecorder recorder;

  recorder.record(100, 110);
  recorder.record(100, 130);
  recorder.record(100, 120);

  const auto stats = recorder.snapshot();

  EXPECT_EQ(stats.count, 3u);
  EXPECT_EQ(stats.invalid_samples, 0u);
  EXPECT_EQ(stats.min_ns, 10u);
  EXPECT_EQ(stats.max_ns, 30u);
  EXPECT_DOUBLE_EQ(static_cast<double>(stats.mean_ns), 20.0);
  EXPECT_EQ(stats.p50_ns, 20u);
  EXPECT_EQ(stats.p90_ns, 30u);
  EXPECT_EQ(stats.p99_ns, 30u);
}

TEST(LatencyRecorderTest, TracksInvalidSamplesWithoutRecordingLatency) {
  LatencyRecorder recorder;

  recorder.record(200, 100);
  recorder.record(100, 105);

  const auto stats = recorder.snapshot();

  EXPECT_EQ(stats.count, 1u);
  EXPECT_EQ(stats.invalid_samples, 1u);
  EXPECT_EQ(stats.min_ns, 5u);
  EXPECT_EQ(stats.max_ns, 5u);
}

TEST(LatencyRecorderTest, RecordsRepeatedDurationAsWeightedSamples) {
  LatencyRecorder recorder;

  recorder.recordDuration(10, 20);

  const auto stats = recorder.snapshot();

  EXPECT_EQ(stats.count, 20u);
  EXPECT_EQ(stats.min_ns, 10u);
  EXPECT_EQ(stats.max_ns, 10u);
  EXPECT_DOUBLE_EQ(static_cast<double>(stats.mean_ns), 10.0);
  EXPECT_EQ(stats.p99_ns, 10u);
}

TEST(LatencyRecorderTest, ReportsEmptyRecorder) {
  LatencyRecorder recorder;
  std::ostringstream out;

  recorder.report(out);

  EXPECT_EQ(out.str(), "latency count=0 invalid=0\n");
}

TEST(LatencyRecorderTest, ResetClearsAllState) {
  LatencyRecorder recorder;

  recorder.record(100, 120);
  recorder.record(200, 100);
  recorder.reset();

  const auto stats = recorder.snapshot();

  EXPECT_EQ(stats.count, 0u);
  EXPECT_EQ(stats.invalid_samples, 0u);
  EXPECT_EQ(stats.min_ns, 0u);
  EXPECT_EQ(stats.max_ns, 0u);
}
