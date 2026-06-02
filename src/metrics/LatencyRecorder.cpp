#include "astra/metrics/LatencyRecorder.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>

void LatencyRecorder::record(std::uint64_t start_ns, std::uint64_t end_ns) {
  if (end_ns < start_ns) {
    ++invalid_samples_;
    return;
  }

  recordDuration(end_ns - start_ns);
}

void LatencyRecorder::recordDuration(std::uint64_t duration_ns) {
  ++buckets_[bucketIndex(duration_ns)];
  ++count_;
  total_ns_ += duration_ns;
  min_ns_ = std::min(min_ns_, duration_ns);
  max_ns_ = std::max(max_ns_, duration_ns);
}

void LatencyRecorder::reset() {
  buckets_.fill(0);
  count_ = 0;
  invalid_samples_ = 0;
  min_ns_ = kNoMin;
  max_ns_ = 0;
  total_ns_ = 0;
}

std::uint64_t LatencyRecorder::percentile(double percentile) const {
  if (count_ == 0) {
    return 0;
  }

  if (percentile <= 0.0) {
    return min_ns_;
  }

  if (percentile >= 100.0) {
    return max_ns_;
  }

  const long double target =
      static_cast<long double>(count_) *
      (static_cast<long double>(percentile) / 100.0L);
  const auto rank = static_cast<std::uint64_t>(std::ceil(target));
  return valueAtRank(std::max<std::uint64_t>(rank, 1));
}

LatencyRecorder::Snapshot LatencyRecorder::snapshot() const {
  Snapshot snapshot;
  snapshot.count = count_;
  snapshot.invalid_samples = invalid_samples_;

  if (count_ == 0) {
    return snapshot;
  }

  snapshot.min_ns = min_ns_;
  snapshot.max_ns = max_ns_;
  snapshot.mean_ns =
      static_cast<long double>(total_ns_) / static_cast<long double>(count_);
  snapshot.p50_ns = percentile(50.0);
  snapshot.p90_ns = percentile(90.0);
  snapshot.p99_ns = percentile(99.0);
  snapshot.p999_ns = percentile(99.9);
  snapshot.p9999_ns = percentile(99.99);
  return snapshot;
}

void LatencyRecorder::report() const { report(std::cout); }

void LatencyRecorder::report(std::ostream &out) const {
  const Snapshot stats = snapshot();
  out << "latency count=" << stats.count
      << " invalid=" << stats.invalid_samples;

  if (stats.count == 0) {
    out << '\n';
    return;
  }

  out << " min_ns=" << stats.min_ns << " max_ns=" << stats.max_ns
      << " mean_ns=" << std::fixed << std::setprecision(2)
      << static_cast<double>(stats.mean_ns) << std::defaultfloat
      << " p50_ns=" << stats.p50_ns << " p90_ns=" << stats.p90_ns
      << " p99_ns=" << stats.p99_ns << " p99.9_ns=" << stats.p999_ns
      << " p99.99_ns=" << stats.p9999_ns << '\n';
}

std::size_t LatencyRecorder::bucketIndex(std::uint64_t latency_ns) {
  if (latency_ns < kExactBucketCount) {
    return static_cast<std::size_t>(latency_ns);
  }

  latency_ns -= kExactBucketCount;
  if (latency_ns < kFineRangeNs) {
    return kExactBucketCount +
           static_cast<std::size_t>(latency_ns / kFineBucketWidthNs);
  }

  latency_ns -= kFineRangeNs;
  if (latency_ns < kCoarseRangeNs) {
    return kExactBucketCount + kFineBucketCount +
           static_cast<std::size_t>(latency_ns / kCoarseBucketWidthNs);
  }

  return kOverflowBucket;
}

std::uint64_t
LatencyRecorder::bucketUpperBoundNs(std::size_t bucket_index) {
  if (bucket_index < kExactBucketCount) {
    return static_cast<std::uint64_t>(bucket_index);
  }

  bucket_index -= kExactBucketCount;
  if (bucket_index < kFineBucketCount) {
    return kExactBucketCount +
           (static_cast<std::uint64_t>(bucket_index + 1) *
            kFineBucketWidthNs) -
           1;
  }

  bucket_index -= kFineBucketCount;
  if (bucket_index < kCoarseBucketCount) {
    return kExactBucketCount + kFineRangeNs +
           (static_cast<std::uint64_t>(bucket_index + 1) *
            kCoarseBucketWidthNs) -
           1;
  }

  return std::numeric_limits<std::uint64_t>::max();
}

std::uint64_t LatencyRecorder::valueAtRank(std::uint64_t rank) const {
  std::uint64_t cumulative = 0;

  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    cumulative += buckets_[i];
    if (cumulative >= rank) {
      return i == kOverflowBucket ? max_ns_ : bucketUpperBoundNs(i);
    }
  }

  return max_ns_;
}
