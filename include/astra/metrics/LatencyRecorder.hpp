#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>

class LatencyRecorder {
public:
  struct Snapshot {
    std::uint64_t count{0};
    std::uint64_t invalid_samples{0};
    std::uint64_t min_ns{0};
    std::uint64_t max_ns{0};
    long double mean_ns{0.0L};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t p9999_ns{0};
  };

  void record(std::uint64_t start_ns, std::uint64_t end_ns);
  void recordDuration(std::uint64_t duration_ns);
  void recordDuration(std::uint64_t duration_ns,
                      std::uint64_t sample_count);
  void reset();

  [[nodiscard]] std::uint64_t count() const { return count_; }
  [[nodiscard]] std::uint64_t invalidSamples() const {
    return invalid_samples_;
  }

  [[nodiscard]] std::uint64_t percentile(double percentile) const;
  [[nodiscard]] Snapshot snapshot() const;

  void report() const;
  void report(std::ostream &out) const;

private:
  static constexpr std::size_t kExactBucketCount = 1024;
  static constexpr std::size_t kFineBucketCount = 4096;
  static constexpr std::size_t kCoarseBucketCount = 4096;
  static constexpr std::uint64_t kFineBucketWidthNs = 16;
  static constexpr std::uint64_t kCoarseBucketWidthNs = 1024;
  static constexpr std::uint64_t kFineRangeNs =
      kFineBucketCount * kFineBucketWidthNs;
  static constexpr std::uint64_t kCoarseRangeNs =
      kCoarseBucketCount * kCoarseBucketWidthNs;
  static constexpr std::size_t kOverflowBucket =
      kExactBucketCount + kFineBucketCount + kCoarseBucketCount;
  static constexpr std::size_t kBucketCount = kOverflowBucket + 1;

  static constexpr std::uint64_t kNoMin =
      std::numeric_limits<std::uint64_t>::max();

  static std::size_t bucketIndex(std::uint64_t latency_ns);
  static std::uint64_t bucketUpperBoundNs(std::size_t bucket_index);

  [[nodiscard]] std::uint64_t valueAtRank(std::uint64_t rank) const;

  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_{0};
  std::uint64_t invalid_samples_{0};
  std::uint64_t min_ns_{kNoMin};
  std::uint64_t max_ns_{0};
  std::uint64_t total_ns_{0};
};
