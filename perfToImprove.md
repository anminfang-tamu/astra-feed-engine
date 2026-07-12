# Performance Review and Optimization Status

This review treats AstraFeed as a long-running ITCH market-data engine, not as
an optimizer for one captured trading day. The design target is bounded or
expected O(1) book mutation, fixed storage after readiness, one authoritative
copy of each index, and explicit failure when a configured envelope is too
small.

## Implemented architecture

### One authoritative full-64-bit order-reference directory

`OrderRefDirectory` is now the only `order_ref -> order` index.

- It accepts every nonzero `uint64_t` ITCH order reference.
- It is a fixed, pre-touched, half-loaded open-addressed table.
- Entries are 16 bytes and contain the full reference plus stock-locate/order
  handle.
- Backward-shift deletion avoids tombstones.
- Insert, replace, erase, and probe failures are counted.
- The old dense-direct/per-book `OrderIdMap` fallback and its tests were
  removed.

The default 16,777,216 slots consume 256 MiB and allow 8,388,608 concurrent
live mappings. This is configurable with `ASTRA_ORDER_DIRECTORY_SLOTS`.

### One global fixed order arena

All books share `OrderArena`; there are no per-symbol order pools or liquidity
tier guesses.

- `Order` is exactly 32 bytes, so two records fit in one 64-byte cache line.
- The record retains `price` for future strategy use.
- The arena owns one contiguous order vector, a fixed free-index stack, and a
  compact one-bit occupancy map.
- Allocation and release are O(1), never resize, and expose exhaustion,
  high-watermark, invalid-release, and double-release counters.
- Global order indices are stable and are used directly by the directory and
  FIFO price-level links.

The default order capacity equals the directory live-entry limit and can be
reduced explicitly with `ASTRA_ORDER_POOL_CAPACITY`.

### Complete ITCH price domain without a second ordered index

The old centered 65,536-slot price window was incorrect for a general ITCH
feed and could silently reject valid far-apart prices. It was replaced by a
four-byte radix/bitmap index over the complete `uint32_t` raw-price domain.

- Every lookup traverses a fixed four-byte path.
- Bid and ask occupancy are independent while topology is shared.
- Existing-order cancel, execute, and delete use the order's direct 32-bit
  level handle.
- Best-price and top-ten traversal use the same radix state; there is no tree or
  duplicate sorted container.
- Empty levels, leaves, and nodes return to fixed free lists.
- Pool creation is atomic: an exhausted pool publishes no partial path.
- Aggregate level quantity, top-of-book quantity, and book-update quantity are
  64-bit.
- Price zero and `UINT32_MAX` are both valid; explicit presence bits distinguish
  an empty side from price zero.

Default shared capacities are 163,840 internal nodes, 1,048,576 leaves, and
2,097,152 occupied side/price levels. Every pool has usage, high-watermark, and
exhaustion metrics.

### Readiness and hot-path allocation boundary

The order directory, order arena, price pools, free lists, and occupancy maps
are allocated and page-touched before packet processing. Stock Directory
messages identify the book universe; Start of System Hours constructs the
registered book roots and seals that universe. A late unknown book is rejected
and counted rather than allocated during live processing.

Capacity failure latches the affected book locally invalid. No operation moves
to a fallback data structure, and shutdown metrics make the failure visible.

### Decoder and measurement cleanup

- A clean in-sequence packet no longer probes the roughly 2 GiB gap table when
  it is empty.
- Repeated stale-gap logging is suppressed.
- The decoder no longer re-decodes and stores an unused stock-locate list or a
  duplicate channel-phase copy for every message.
- `ASTRA_LATENCY_METRICS=off` now disables receiver `rdtsc` capture, including
  kernel UDP, batched UDP, DPDK, and file replay sources. The decoder therefore
  performs no TSC conversion in throughput mode.
- When latency is enabled, TSC calibration completes before engine readiness
  instead of stalling the first measured packet.

### Reproducible build gate

- Fresh single-config CMake builds default to `Release`.
- `ASTRA_ENABLE_IPO=ON` enables compiler-checked IPO/LTO.
- UDP and DPDK wrappers always reconfigure and incrementally rebuild before a
  run, then print Git SHA, dirty/clean state, build type, and IPO mode.
- Historical README transport rows are marked as cross-revision references and
  must not be compared as if they came from one binary.

## Verification completed

Focused sanitizer coverage passes:

- 72 order-arena, order-directory, price-index, manager, and order-book tests
  under ASan and UBSan.
- 16 ITCH parser integration tests under ASan and UBSan.
- Randomized differential tests cover directory insert/erase/replace, arena
  allocation/release, and sparse price ordering/reclamation.
- Boundary tests cover order refs through `UINT64_MAX`, prices from zero through
  `UINT32_MAX`, byte-boundary traversal, same price on both sides, replacement,
  pool exhaustion, rollback, and aggregate quantity above `UINT32_MAX`.

The full local 2019 trace gate also passes:

```text
records=368366634
bytes=11245883092
replay_seconds=86.597
records_per_second=4253804.594
order_high_watermark=1742866
price_internal_node_high_watermark=71065
price_leaf_high_watermark=587520
price_level_high_watermark=707130
all failure counters=0
final live orders=0
```

This trace is a correctness and capacity observation, not a production sizing
proof. Defaults retain headroom and remain configurable because another day,
venue, or symbol mix can have a different concurrent shape.

## Remaining recommendations

### P0: establish a same-binary network baseline

Rerun kernel `recv`, `recvmmsg`, and DPDK on one clean Git SHA with verified
Release flags, optional IPO held constant, identical replay shape, and these
acceptance gates:

- sender and receiver final sequences match;
- channel status is `Good`;
- kernel/DPDK drops are zero;
- every book/directory/order/price failure counter is zero.

Collect `perf stat` and `perf record` after readiness: cycles, instructions,
IPC, branch misses, LLC misses, dTLB misses, page faults, and top call stacks.
Do not claim a transport winner from the existing cross-revision README rows.

### P1: size the gap-recovery buffer from observed outages

`GapBuffer` still reserves about 2.03 GiB for 1,048,576 full packet slots.
Skipping the empty lookup removed normal-path cache pollution, but the storage
envelope remains large. Measure maximum recoverable gap depth and recovery time
on real feeds, then make capacity explicit and preallocated. Keep failure
terminal and counted; do not add dynamic growth.

### P1: profile repeated handle validation before weakening it

Manager resolution validates the directory handle, and indexed book mutations
validate the same order again. IPO may remove some overhead, and the second
check protects against stale/corrupt handles. Measure first. If still material,
introduce an internal resolved-order token or a distinct stale-handle mutation
result rather than simply deleting safety checks.

### P1: add message-type and operation microbenchmarks

The full-file benchmark measures aggregate parser/book throughput. Add fixed
traces for add, cancel, execute, delete, replace-at-same-price, replace-to-new
path, and last-level reclamation. Report distributions as well as throughput;
pool reuse paths can have different cache costs from first-use paths.

### P2: process receive batches as batches

`UdpBatchReceiver` uses `recvmmsg`, but the current engine compatibility path
returns one saved datagram per virtual `next()` call. A batch-aware engine loop
can amortize dispatch and loop overhead while preserving packet sequence order.
Compare it only after the new book path is profiled.

### P2: residency and NUMA guarantees

Page touching prevents first-touch faults but does not guarantee pages cannot
be reclaimed. On the Linux deployment host, measure and optionally support
`mlock`/`mlock2`, transparent or explicit huge pages, and NUMA-local allocation.
Startup should fail clearly when a requested strict residency policy cannot be
honored.

### P3: specialize the live engine loop only if dispatch remains visible

The source and processor interfaces still use virtual dispatch once per packet.
Keep them for tests and configuration. Add a concrete or templated live loop
only if profiles show dispatch is material after book, receive, and memory
costs are addressed.
