#include "astra/book/BookManager.hpp"
#include "astra/core/Time.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

volatile std::uint64_t g_checksum = 0;
constexpr std::uint32_t kDirectPricePages = 64;
constexpr std::uint32_t kWidePartialPageLimit = 4'096;
constexpr std::uint32_t kCrossPageBestLimit = 4'096;
constexpr std::string_view kUsage =
    "usage: astra_order_book_benchmark [--iterations=N] "
    "[--gate=WORKLOAD:P50:P99:P99.9]... "
    "[--max-direct-p50-ns=N] [--max-direct-p99-ns=N] "
    "[--max-direct-p99-9-ns=N]";

enum class Workload : std::size_t {
  DirectPartialExecute,
  DirectPartialExecuteWidePages,
  DirectPartialCancelWidePages,
  AddExistingLevel,
  DeletePopulatedLevel,
  ReplaceCrossPage,
  RemoveCurrentBest,
  RemoveCrossPageBest,
  FallbackPartialExecute,
  FallbackFourSlotMiss,
  Count,
};

constexpr std::array<std::string_view,
                     static_cast<std::size_t>(Workload::Count)>
    kWorkloadNames = {
        "direct_partial_execute",
        "direct_partial_execute_wide_pages",
        "direct_partial_cancel_wide_pages",
        "add_existing_level",
        "delete_populated_level",
        "replace_cross_page",
        "remove_current_best",
        "remove_cross_page_best",
        "fallback_partial_execute",
        "fallback_four_slot_miss",
};

constexpr std::size_t workloadIndex(Workload workload) noexcept {
  return static_cast<std::size_t>(workload);
}

constexpr std::string_view workloadName(Workload workload) noexcept {
  return kWorkloadNames[workloadIndex(workload)];
}

Workload findWorkload(std::string_view name) noexcept {
  const auto found = std::find(kWorkloadNames.begin(), kWorkloadNames.end(),
                               name);
  if (found == kWorkloadNames.end())
    return Workload::Count;
  return static_cast<Workload>(found - kWorkloadNames.begin());
}

struct LatencyGate {
  Workload workload{Workload::Count};
  std::uint64_t maximum_p50_ns{0};
  std::uint64_t maximum_p99_ns{0};
  std::uint64_t maximum_p999_ns{0};
};

std::uint64_t nextPowerOfTwo(std::uint64_t value) {
  if (value <= 2)
    return 2;
  --value;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value |= value >> 32;
  return value + 1;
}

struct Options {
  std::uint32_t iterations{100'000};
  std::vector<LatencyGate> latency_gates;
};

std::uint64_t parseU64(std::string_view value, const char *name) {
  std::uint64_t parsed = 0;
  const char *const begin = value.data();
  const char *const end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (value.empty() || result.ec != std::errc{} || result.ptr != end)
    throw std::invalid_argument(std::string("invalid ") + name);
  return parsed;
}

void validateLatencyGate(const LatencyGate &gate, bool require_all) {
  if (require_all && (gate.maximum_p50_ns == 0 ||
                      gate.maximum_p99_ns == 0 ||
                      gate.maximum_p999_ns == 0)) {
    throw std::invalid_argument("named gate thresholds must be positive");
  }

  const bool p50_p99_inverted =
      gate.maximum_p50_ns != 0 && gate.maximum_p99_ns != 0 &&
      gate.maximum_p50_ns > gate.maximum_p99_ns;
  const bool p99_p999_inverted =
      gate.maximum_p99_ns != 0 && gate.maximum_p999_ns != 0 &&
      gate.maximum_p99_ns > gate.maximum_p999_ns;
  const bool p50_p999_inverted =
      gate.maximum_p50_ns != 0 && gate.maximum_p999_ns != 0 &&
      gate.maximum_p50_ns > gate.maximum_p999_ns;
  if (p50_p99_inverted || p99_p999_inverted || p50_p999_inverted) {
    throw std::invalid_argument(
        "gate thresholds must be monotonic: p50 <= p99 <= p99.9");
  }
}

LatencyGate parseLatencyGate(std::string_view specification) {
  const std::size_t first = specification.find(':');
  const std::size_t second =
      first == std::string_view::npos
          ? std::string_view::npos
          : specification.find(':', first + 1);
  const std::size_t third =
      second == std::string_view::npos
          ? std::string_view::npos
          : specification.find(':', second + 1);
  if (first == std::string_view::npos || second == std::string_view::npos ||
      third == std::string_view::npos ||
      specification.find(':', third + 1) != std::string_view::npos) {
    throw std::invalid_argument(
        "gate must have WORKLOAD:P50:P99:P99.9 format");
  }

  const std::string_view name = specification.substr(0, first);
  const Workload workload = findWorkload(name);
  if (workload == Workload::Count) {
    throw std::invalid_argument("unknown gate workload: " +
                                std::string(name));
  }

  LatencyGate gate{
      workload,
      parseU64(specification.substr(first + 1, second - first - 1),
               "gate p50 threshold"),
      parseU64(specification.substr(second + 1, third - second - 1),
               "gate p99 threshold"),
      parseU64(specification.substr(third + 1), "gate p99.9 threshold"),
  };
  validateLatencyGate(gate, true);
  return gate;
}

void addLatencyGate(Options &options, LatencyGate gate) {
  const auto duplicate =
      std::find_if(options.latency_gates.begin(), options.latency_gates.end(),
                   [&](const LatencyGate &existing) {
                     return existing.workload == gate.workload;
                   });
  if (duplicate != options.latency_gates.end()) {
    throw std::invalid_argument("duplicate gate for workload: " +
                                std::string(workloadName(gate.workload)));
  }
  options.latency_gates.push_back(gate);
}

Options parseOptions(int argc, char **argv) {
  Options options;
  LatencyGate legacy_direct_gate{Workload::DirectPartialExecute};
  bool legacy_p50_seen = false;
  bool legacy_p99_seen = false;
  bool legacy_p999_seen = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    constexpr std::string_view iterations_prefix = "--iterations=";
    constexpr std::string_view gate_prefix = "--gate=";
    constexpr std::string_view p50_prefix = "--max-direct-p50-ns=";
    constexpr std::string_view p99_prefix = "--max-direct-p99-ns=";
    constexpr std::string_view p999_prefix = "--max-direct-p99-9-ns=";
    if (argument.starts_with(iterations_prefix)) {
      const std::uint64_t iterations =
          parseU64(argument.substr(iterations_prefix.size()), "iterations");
      if (iterations == 0 || iterations > 1'000'000)
        throw std::invalid_argument("iterations must be in [1, 1000000]");
      options.iterations = static_cast<std::uint32_t>(iterations);
    } else if (argument.starts_with(gate_prefix)) {
      addLatencyGate(
          options,
          parseLatencyGate(argument.substr(gate_prefix.size())));
    } else if (argument.starts_with(p50_prefix)) {
      if (legacy_p50_seen)
        throw std::invalid_argument("duplicate direct p50 gate");
      legacy_p50_seen = true;
      legacy_direct_gate.maximum_p50_ns =
          parseU64(argument.substr(p50_prefix.size()), "direct p50 gate");
    } else if (argument.starts_with(p99_prefix)) {
      if (legacy_p99_seen)
        throw std::invalid_argument("duplicate direct p99 gate");
      legacy_p99_seen = true;
      legacy_direct_gate.maximum_p99_ns =
          parseU64(argument.substr(p99_prefix.size()), "direct p99 gate");
    } else if (argument.starts_with(p999_prefix)) {
      if (legacy_p999_seen)
        throw std::invalid_argument("duplicate direct p99.9 gate");
      legacy_p999_seen = true;
      legacy_direct_gate.maximum_p999_ns =
          parseU64(argument.substr(p999_prefix.size()), "direct p99.9 gate");
    } else {
      throw std::invalid_argument("unknown benchmark option: " +
                                  std::string(argument));
    }
  }

  const bool legacy_gate_seen =
      legacy_p50_seen || legacy_p99_seen || legacy_p999_seen;
  if (legacy_gate_seen) {
    validateLatencyGate(legacy_direct_gate, false);
    if (std::any_of(options.latency_gates.begin(),
                    options.latency_gates.end(), [](const LatencyGate &gate) {
                      return gate.workload == Workload::DirectPartialExecute;
                    })) {
      throw std::invalid_argument(
          "duplicate gate for workload: direct_partial_execute");
    }
    if (legacy_direct_gate.maximum_p50_ns != 0 ||
        legacy_direct_gate.maximum_p99_ns != 0 ||
        legacy_direct_gate.maximum_p999_ns != 0) {
      options.latency_gates.push_back(legacy_direct_gate);
    }
  }
  return options;
}

std::uint64_t splitMix64(std::uint64_t &state) noexcept {
  state += 0x9e3779b97f4a7c15ull;
  std::uint64_t value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

std::vector<std::uint32_t>
deterministicAccessOrder(std::uint32_t iterations) {
  std::vector<std::uint32_t> order(iterations);
  std::iota(order.begin(), order.end(), 0);
  std::uint64_t state = 0x61737472612d626full;
  for (std::size_t remaining = order.size(); remaining > 1; --remaining) {
    const std::size_t other =
        static_cast<std::size_t>(splitMix64(state) % remaining);
    std::swap(order[remaining - 1], order[other]);
  }
  return order;
}

std::uint32_t scatteredDirectPrice(std::uint32_t ordinal) noexcept {
  // A bounded 64-page working set (128 MiB of side-separated level storage)
  // avoids both the old single-level hot case and unbounded physical memory.
  const std::uint32_t page_index = (ordinal * 37u + 11u) & 63u;
  const std::uint32_t level_index = (ordinal * 40'503u + 97u) & 0xffffu;
  return (page_index << 16) | level_index;
}

BookManagerConfig directConfig(std::uint64_t order_slots,
                               std::uint32_t price_pages = 8) {
  return {{nextPowerOfTwo(order_slots), 1u << 12, true},
          {price_pages, false}};
}

void require(astra::book::MutationResult result, const char *operation) {
  if (result != astra::book::MutationResult::Applied) {
    throw std::runtime_error(std::string(operation) + ": " +
                             std::string(astra::book::mutationResultName(
                                 result)));
  }
}

struct Distribution {
  std::uint64_t p50;
  std::uint64_t p90;
  std::uint64_t p99;
  std::uint64_t p999;
  std::uint64_t maximum;
};

struct FallbackBenchmarkResult {
  Distribution distribution;
  std::uint64_t primary{0};
  std::uint64_t secondary{0};
  std::array<std::uint64_t, astra::book::OrderTable::kFallbackWays>
      primary_ways{};
  std::array<std::uint64_t, astra::book::OrderTable::kFallbackWays>
      secondary_ways{};
};

std::uint64_t percentile(const std::vector<std::uint64_t> &values,
                         std::uint32_t numerator,
                         std::uint32_t denominator) {
  const std::size_t rank =
      (values.size() * static_cast<std::size_t>(numerator) + denominator - 1) /
      denominator;
  const std::size_t index = std::min(values.size() - 1, rank - 1);
  return values[index];
}

Distribution summarize(std::vector<std::uint64_t> ticks) {
  for (std::uint64_t &sample : ticks) {
    sample = elapsedRdtscNs(0, sample);
  }
  std::sort(ticks.begin(), ticks.end());
  return {percentile(ticks, 50, 100), percentile(ticks, 90, 100),
          percentile(ticks, 99, 100), percentile(ticks, 999, 1000),
          ticks.back()};
}

void print(std::string_view name, const Distribution &distribution,
           std::size_t iterations) {
  std::cout << "workload=" << name << " iterations=" << iterations
            << " p50_ns=" << distribution.p50
            << " p90_ns=" << distribution.p90
            << " p99_ns=" << distribution.p99
            << " p99_9_ns=" << distribution.p999
            << " max_ns=" << distribution.maximum << '\n';
}

using WorkloadDistributions =
    std::array<Distribution, static_cast<std::size_t>(Workload::Count)>;

void storeAndPrint(WorkloadDistributions &distributions, Workload workload,
                   Distribution distribution, std::size_t iterations) {
  Distribution &stored = distributions[workloadIndex(workload)];
  stored = distribution;
  print(workloadName(workload), stored, iterations);
}

bool reportGateFailure(std::string_view workload, std::string_view percentile,
                       std::uint64_t measured_ns,
                       std::uint64_t maximum_ns) {
  if (maximum_ns == 0 || measured_ns <= maximum_ns)
    return false;
  std::cerr << "latency gate failed: workload=" << workload
            << " percentile=" << percentile
            << " measured_ns=" << measured_ns
            << " maximum_ns=" << maximum_ns << '\n';
  return true;
}

bool latencyGatesFailed(const std::vector<LatencyGate> &gates,
                        const WorkloadDistributions &distributions) {
  bool failed = false;
  for (const LatencyGate &gate : gates) {
    const Distribution &distribution =
        distributions[workloadIndex(gate.workload)];
    const std::string_view name = workloadName(gate.workload);
    failed = reportGateFailure(name, "p50", distribution.p50,
                               gate.maximum_p50_ns) ||
             failed;
    failed = reportGateFailure(name, "p99", distribution.p99,
                               gate.maximum_p99_ns) ||
             failed;
    failed = reportGateFailure(name, "p99.9", distribution.p999,
                               gate.maximum_p999_ns) ||
             failed;
  }
  return failed;
}

void requireLatencyGateClock(const Options &options) {
#if !defined(__x86_64__) && !defined(__i386__)
  if (!options.latency_gates.empty()) {
    throw std::runtime_error(
        "latency gates require x86 RDTSCP; portable-clock results cannot "
        "enforce thresholds");
  }
#else
  (void)options;
#endif
}

template <typename Operation>
Distribution measure(
    std::uint32_t iterations, Operation operation,
    astra::book::MutationResult expected =
        astra::book::MutationResult::Applied) {
  std::vector<std::uint64_t> deltas(iterations);
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const std::uint64_t start = rdtsc();
    const auto result = operation(index);
    const std::uint64_t end = rdtsc();
    if (result != expected) {
      throw std::runtime_error(
          "timed mutation returned " +
          std::string(astra::book::mutationResultName(result)) +
          ", expected " +
          std::string(astra::book::mutationResultName(expected)));
    }
    deltas[index] = end - start;
  }
  return summarize(std::move(deltas));
}

Distribution directPartialExecute(std::uint32_t iterations) {
  BookManager manager(directConfig(iterations + 2, kDirectPricePages));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");
  for (std::uint32_t ordinal = 0; ordinal < iterations; ++ordinal) {
    require(manager.addOrder(7, static_cast<std::uint64_t>(ordinal) + 1,
                             scatteredDirectPrice(ordinal), 2, 'B'),
            "setup add");
  }
  const std::vector<std::uint32_t> access_order =
      deterministicAccessOrder(iterations);

  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    const std::uint64_t reference =
        static_cast<std::uint64_t>(access_order[index]) + 1;
    return manager.executeOrder(7, reference, 1);
  });
  g_checksum = g_checksum + book->getTopOfBook().bid_qty;
  return result;
}

template <typename Mutation>
Distribution directPartialWidePages(std::uint32_t requested_iterations,
                                    Mutation mutation) {
  // Keep exactly one live order in each page. Random timed reference order
  // therefore exercises the direct OrderState -> page-owner -> level path
  // across thousands of pages without pulling page/book summary bitmaps into
  // the partial-mutation working set.
  const std::uint32_t iterations =
      std::min(requested_iterations, kWidePartialPageLimit);
  BookManager manager(directConfig(iterations + 2, iterations));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");

  for (std::uint32_t ordinal = 0; ordinal < iterations; ++ordinal) {
    // Odd multipliers are permutations modulo 2^16, so every ordinal in this
    // bounded prefix selects a different page and level without a range table.
    const std::uint32_t page_index =
        (ordinal * 40'503u + 97u) & 0xffffu;
    const std::uint32_t level_index =
        (ordinal * 25'173u + 211u) & 0xffffu;
    const std::uint32_t price = (page_index << 16) | level_index;
    require(manager.addOrder(7, static_cast<std::uint64_t>(ordinal) + 1,
                             price, 2, 'B'),
            "wide partial setup add");
  }

  const std::vector<std::uint32_t> access_order =
      deterministicAccessOrder(iterations);
  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    const std::uint64_t reference =
        static_cast<std::uint64_t>(access_order[index]) + 1;
    return mutation(manager, reference);
  });
  g_checksum = g_checksum + book->getTopOfBook().bid_qty;
  return result;
}

Distribution addExistingLevel(std::uint32_t iterations) {
  BookManager manager(directConfig(iterations + 3));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");
  require(manager.addOrder(7, 1, 1'000'000, 1, 'B'), "prime page");

  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    return manager.addOrder(7, static_cast<std::uint64_t>(index) + 2,
                            1'000'000, 10, 'B');
  });
  g_checksum = g_checksum + book->getTopOfBook().bid_qty;
  return result;
}

Distribution deletePopulatedLevel(std::uint32_t iterations) {
  BookManager manager(directConfig(iterations + 2));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");
  for (std::uint32_t ref = 1; ref <= iterations; ++ref)
    require(manager.addOrder(7, ref, 1'000'000, 10, 'S'), "setup add");

  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    return manager.deleteOrder(7, static_cast<std::uint64_t>(index) + 1);
  });
  g_checksum = g_checksum + book->liveOrderCount();
  return result;
}

Distribution replaceAcrossPage(std::uint32_t iterations) {
  BookManager manager(directConfig(std::uint64_t{iterations} * 2 + 2));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");
  for (std::uint32_t ref = 1; ref <= iterations; ++ref)
    require(manager.addOrder(7, ref, 100, 10, 'B'), "setup add");
  require(manager.addOrder(7, std::uint64_t{iterations} * 2 + 1,
                           65'600, 1, 'B'),
          "prime destination page");

  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    const std::uint64_t old_ref = static_cast<std::uint64_t>(index) + 1;
    return manager.replaceOrder(7, old_ref, old_ref + iterations, 65'600,
                                20);
  });
  g_checksum = g_checksum + book->getTopOfBook().bid_qty;
  return result;
}

Distribution removeEveryBest(std::uint32_t requested_iterations) {
  const std::uint32_t iterations =
      std::min<std::uint32_t>(requested_iterations, 60'000);
  BookManager manager(directConfig(iterations + 2));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");
  for (std::uint32_t ref = 1; ref <= iterations; ++ref)
    require(manager.addOrder(7, ref, ref, 1, 'B'), "setup level");

  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    return manager.deleteOrder(7,
                               static_cast<std::uint64_t>(iterations - index));
  });
  g_checksum = g_checksum + book->liveOrderCount();
  return result;
}

Distribution removeCrossPageBest(std::uint32_t requested_iterations) {
  // One occupied level per page forces every timed deletion through the
  // cross-page hierarchy: empty the current page, remove that page from the
  // book bitmap, then find and resolve the next occupied page. Singleton pages
  // intentionally avoid their per-page bitmap. Allocate pages
  // in a fixed-seed permutation so price order and arena-handle order are not
  // accidentally correlated. Keep the resident setup bounded while retaining
  // enough samples for p99.9.
  const std::uint32_t iterations =
      std::min(requested_iterations, kCrossPageBestLimit);
  BookManager manager(directConfig(iterations + 2, iterations));
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");

  constexpr std::uint32_t level_index = 0x1234u;
  const std::vector<std::uint32_t> allocation_order =
      deterministicAccessOrder(iterations);
  std::vector<std::uint64_t> reference_by_page(iterations);
  for (std::uint32_t ordinal = 0; ordinal < iterations; ++ordinal) {
    const std::uint32_t page_index = allocation_order[ordinal];
    const std::uint32_t price = (page_index << 16) | level_index;
    const std::uint64_t reference =
        static_cast<std::uint64_t>(ordinal) + 1;
    require(manager.addOrder(7, reference,
                             price, 1, 'B'),
            "setup cross-page level");
    reference_by_page[page_index] = reference;
  }

  const Distribution result = measure(iterations, [&](std::uint32_t index) {
    const std::uint32_t page_index = iterations - index - 1;
    return manager.deleteOrder(7, reference_by_page[page_index]);
  });
  g_checksum = g_checksum + book->liveOrderCount();
  return result;
}

FallbackBenchmarkResult fallbackPartialExecute(std::uint32_t iterations) {
  const auto buckets = static_cast<std::uint32_t>(
      nextPowerOfTwo(std::uint64_t{iterations} * 4));
  BookManager manager(
      BookManagerConfig{{8, buckets, true}, {2, false}});
  OrderBook *book = manager.getOrCreate(7);
  if (book == nullptr)
    throw std::runtime_error("book preparation failed");
  constexpr std::uint64_t base = std::uint64_t{1} << 63;
  std::vector<std::uint64_t> references;
  references.reserve(iterations);
  FallbackBenchmarkResult benchmark{};
  std::uint64_t candidate = base;
  const std::uint64_t maximum_attempts =
      static_cast<std::uint64_t>(iterations) * 32u + 1'024u;
  std::uint64_t attempts = 0;
  while (references.size() < iterations && attempts < maximum_attempts) {
    // The two-choice/two-way table intentionally rejects a key when both of
    // its fixed candidate buckets are full. Select insertable keys during
    // untimed setup so the workload measures successful fallback lookups,
    // not a probabilistic table-fill experiment.
    const auto reservation = manager.orderTable().reserve(candidate);
    if (reservation.valid()) {
      require(manager.addOrder(7, candidate, 100, 2, 'S'),
              "fallback setup add");
      references.push_back(candidate);
      const auto lookup = manager.orderTable().find(candidate);
      if (!lookup.found() || lookup.path != reservation.path)
        throw std::runtime_error("fallback setup lookup mismatch");
      if (lookup.path == astra::book::LookupPath::FallbackPrimary) {
        ++benchmark.primary;
        ++benchmark.primary_ways[reservation.way];
      } else if (lookup.path == astra::book::LookupPath::FallbackSecondary) {
        ++benchmark.secondary;
        ++benchmark.secondary_ways[reservation.way];
      } else {
        throw std::runtime_error("fallback setup selected a non-fallback path");
      }
    }
    ++candidate;
    ++attempts;
  }
  if (references.size() != iterations)
    throw std::runtime_error("fallback setup exceeded its bounded attempt limit");

  const std::vector<std::uint32_t> access_order =
      deterministicAccessOrder(iterations);

  benchmark.distribution = measure(iterations, [&](std::uint32_t index) {
    return manager.executeOrder(7, references[access_order[index]], 1);
  });
  g_checksum = g_checksum + book->getTopOfBook().ask_qty;
  return benchmark;
}

Distribution fallbackWorstCaseMiss(std::uint32_t iterations) {
  static_assert(astra::book::OrderTable::kFallbackCandidateBuckets == 2);
  static_assert(astra::book::OrderTable::kFallbackWays == 2);
  static_assert(astra::book::OrderTable::kMaxFallbackSlotsExamined == 4);

  BookManager manager(BookManagerConfig{{8, 2, true}, {1, false}});
  if (manager.getOrCreate(7) == nullptr)
    throw std::runtime_error("book preparation failed");

  constexpr std::uint64_t base = std::uint64_t{1} << 63;
  std::uint64_t candidate = base;
  std::uint32_t inserted = 0;
  for (std::uint32_t attempts = 0; attempts < 1'024 && inserted < 4;
       ++attempts, ++candidate) {
    if (manager.orderTable().reserve(candidate).valid()) {
      require(manager.addOrder(7, candidate, 100, 2, 'S'),
              "fallback miss setup add");
      ++inserted;
    }
  }
  if (inserted != 4)
    throw std::runtime_error("unable to fill both fallback candidate buckets");

  constexpr std::uint64_t miss_base = base + (std::uint64_t{1} << 32);
  const std::vector<std::uint32_t> access_order =
      deterministicAccessOrder(iterations);
  const Distribution result = measure(
      iterations,
      [&](std::uint32_t index) {
        return manager.executeOrder(
            7, miss_base + static_cast<std::uint64_t>(access_order[index]), 1);
      },
      astra::book::MutationResult::OrderNotFound);
  g_checksum = g_checksum + iterations;
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << kUsage << '\n'
                << "Repeat --gate once per workload. Named gates require "
                   "three positive monotonic nanosecond ceilings and x86 "
                   "RDTSCP.\n"
                << "Legacy --max-direct-* options target "
                   "direct_partial_execute; zero disables that individual "
                   "legacy threshold.\n"
                << "Workloads:";
      for (const std::string_view name : kWorkloadNames)
        std::cout << ' ' << name;
      std::cout << '\n';
      return EXIT_SUCCESS;
    }
    const Options options = parseOptions(argc, argv);
    requireLatencyGateClock(options);
    (void)timeCalibration();

    std::cout << "benchmark_config direct_reference_order="
                 "fixed_seed_permutation direct_price_pages="
              << kDirectPricePages
              << " wide_partial_price_pages=" << kWidePartialPageLimit
              << " cross_page_allocation=fixed_seed_permutation"
              << " singleton_page_bitmap=elided\n";
    WorkloadDistributions distributions{};
    storeAndPrint(distributions, Workload::DirectPartialExecute,
                  directPartialExecute(options.iterations),
                  options.iterations);
    const std::uint32_t wide_partial_iterations =
        std::min(options.iterations, kWidePartialPageLimit);
    storeAndPrint(
        distributions, Workload::DirectPartialExecuteWidePages,
        directPartialWidePages(
            options.iterations,
            [](BookManager &manager, std::uint64_t reference) {
              return manager.executeOrder(7, reference, 1);
            }),
        wide_partial_iterations);
    storeAndPrint(
        distributions, Workload::DirectPartialCancelWidePages,
        directPartialWidePages(
            options.iterations,
            [](BookManager &manager, std::uint64_t reference) {
              return manager.cancelShares(7, reference, 1);
            }),
        wide_partial_iterations);
    storeAndPrint(distributions, Workload::AddExistingLevel,
                  addExistingLevel(options.iterations), options.iterations);
    storeAndPrint(distributions, Workload::DeletePopulatedLevel,
                  deletePopulatedLevel(options.iterations),
                  options.iterations);
    storeAndPrint(distributions, Workload::ReplaceCrossPage,
                  replaceAcrossPage(options.iterations), options.iterations);
    storeAndPrint(distributions, Workload::RemoveCurrentBest,
                  removeEveryBest(options.iterations),
                  std::min<std::uint32_t>(options.iterations, 60'000));
    storeAndPrint(distributions, Workload::RemoveCrossPageBest,
                  removeCrossPageBest(options.iterations),
                  std::min(options.iterations, kCrossPageBestLimit));
    const FallbackBenchmarkResult fallback =
        fallbackPartialExecute(options.iterations);
    storeAndPrint(distributions, Workload::FallbackPartialExecute,
                  fallback.distribution, options.iterations);
    std::cout << "fallback_setup"
              << " primary_lookups=" << fallback.primary
              << " secondary_lookups=" << fallback.secondary
              << " primary_way0=" << fallback.primary_ways[0]
              << " primary_way1=" << fallback.primary_ways[1]
              << " secondary_way0=" << fallback.secondary_ways[0]
              << " secondary_way1=" << fallback.secondary_ways[1] << '\n';
    storeAndPrint(distributions, Workload::FallbackFourSlotMiss,
                  fallbackWorstCaseMiss(options.iterations),
                  options.iterations);

    std::cout << "checksum=" << g_checksum << '\n';
    if (latencyGatesFailed(options.latency_gates, distributions))
      return EXIT_FAILURE;
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "order-book benchmark failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
