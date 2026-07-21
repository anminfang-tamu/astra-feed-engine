# Performance Review and Optimization Status

This review treats AstraFeed as a long-running ITCH market-data engine. The
design target is fixed storage after readiness, bounded hot-path work, explicit
capacity failure, and one clear owner for each piece of order-book state.

## Current hybrid architecture

### Symbol-owned order storage and reference index

Each `OrderBook` owns both of the structures needed to resolve and mutate an
order:

- `LocalOrderRefMap`: full nonzero `uint64_t order_ref -> uint32_t local index`.
- `OrderArena`: fixed records, free-index stack, and occupancy bits for that
  symbol only.

`BookManager` no longer owns a global order directory or global order arena. It
routes by the ITCH Stock Locate field, prepares books, aggregates shutdown
statistics, and tracks process-wide live-order high-watermark state. All
reference-map insert/erase/replace transactions now live beside the order and
FIFO mutation inside `OrderBook`.

The local map uses 16-byte entries and reserves four slots per configured
order. Since it enforces a 50% table limit while the order arena is the tighter
limit, actual maximum load is 25%. This is an intentional memory-for-latency
trade: about 64 bytes of map plus 36.125 bytes of arena storage per configured
order for the power-of-two built-in tiers. A custom non-power-of-two capacity
rounds the map to the next power of two and can reserve nearly eight slots per
order in the worst alignment case.

Construction sizes and first-touches every vector. Add, find, cancel, execute,
delete, replace, and release never resize or allocate. Erase performs
backward-shift compaction by copying 16-byte entries within the existing array;
it does not allocate. Permanent tombstones were not selected because a hot
symbol's full-day churn can eventually remove every real Empty terminator and
force capacity-length probes even when few orders are live.

Order references are now scoped internally by `(stock_locate, order_ref)`. This
restores the main-branch ownership model and permits the same numeric reference
in two books even though NASDAQ specifies the reference as day-unique. The
engine therefore trusts that valid-feed invariant instead of enforcing it with
a second global hot-path index. A wrong-locate message invalidates the addressed
book but cannot identify or invalidate the true owner elsewhere.

### Full raw-price domain with shared fixed backing

The centered 65,536-price window from the original main design was not correct
for a general ITCH feed: after choosing a reference price, a valid far-away raw
price could fall outside the window. The hybrid keeps the newer four-byte
radix/bitmap index over the complete `uint32_t` raw-price domain.

- Each book owns its radix root and its bid/ask occupancy and FIFO semantics.
- All books draw nodes, leaves, and level records from one preallocated
  `PriceLevelArena`.
- Price zero and `UINT32_MAX` are valid.
- Bid and ask at the same raw price remain independent.
- Existing-order mutations use the order's direct level handle.
- Best price and top ten traverse the same radix state; there is no second
  ordered container.
- Empty paths return to fixed free lists, and exhausted creation rolls back
  without publishing a partial path.
- Aggregate quantity is 64-bit.

The default shared capacities remain 163,840 internal nodes, 1,048,576 leaves,
and 2,097,152 side/price level records. This backing is still a market-wide
failure domain; counters and health gates expose exhaustion.

### Capacity tiers and readiness

Built-in order capacities are:

| Tier | Orders per book | Approximate local storage |
| --- | ---: | ---: |
| Default | 65,536 | 6.258 MiB |
| Active | 262,144 | 25.031 MiB |
| Hot | 1,048,576 | 100.125 MiB |
| UltraHot | 4,194,304 | 400.5 MiB |

`ASTRA_BOOK_ORDER_CAPACITY` controls the manager default. Explicit per-locate
capacity configuration wins over the compiled ticker tier. The higher-tier
ticker list is still static and should eventually be replaced with measured
deployment configuration.

Stock Directory messages identify the book universe. At Start of System Hours
(`SS`) every registered book is constructed and first-touched synchronously;
the universe is then sealed. Late books are rejected rather than allocated on
the live path. Replay deployments must provide enough `SS` pause for this warmup
and must bind the engine before first-touch to obtain the intended NUMA
placement.

## Correctness and verification status

The focused local build covers:

- full `uint64_t` references and sparse values;
- duplicate, missing, replace, erase, fill/fail/delete/reuse, and randomized
  local-map behavior;
- per-book capacity isolation and identical references across books;
- wrong-locate mutation behavior;
- complete raw prices from zero through `UINT32_MAX`;
- distant prices, radix-byte boundaries, top-ten ordering, and bid/ask
  independence;
- shared price-pool exhaustion, atomic rollback, release, and cross-book reuse;
- aggregate quantity above `UINT32_MAX`;
- parser routing, readiness, sealing, and ticker-tier selection.
- rejection of book messages before the preallocated universe is ready.

On the current ARM macOS development host, affected sources pass strict C++20
syntax checks and the focused book/parser tests can be linked manually. The
full CMake target is intentionally Linux x86-only because `Time.cpp` requires
the RDTSC timing path.

The README latency table and 368,366,634-record replay numbers belong to clean
revision `3e11646f4931`, which used the global reference directory and global
order arena. They do not validate performance of the current hybrid.

## Remaining performance work

### P0: validate the hybrid on the production envelope

Run a clean full-file correctness replay and same-binary DPDK test. Record:

- sender/receiver final sequence equality and `Good` channel health;
- zero transport, book, local-index, order-arena, and price-pool failures;
- RSS, VMA count, tier counts, and total configured local capacity;
- `SS` construction/first-touch duration;
- NUMA residency after warmup;
- latency percentiles and throughput;
- `perf stat` cycles, IPC, branches, LLC misses, dTLB misses, and page faults.

Do not claim parity or superiority over the historical global design until this
same-environment comparison exists.

### P0: preflight readiness before committing memory

The parser now latches any `SS` preparation failure, refuses the Market Hours
transition, and propagates a decoder/channel error. However, allocation still
proceeds book by book, so a failed attempt can leave a partially allocated
sealed universe. Preflight the aggregate tier/capacity budget and report the
failed locate before committing the large first-touch phase. Startup and
benchmark initialization measurements should include this work.

### P1: externalize the symbol capacity profile

Load measured per-symbol high-watermarks with explicit headroom instead of
depending on a compiled ticker list. Validate the complete file before
allocation and report ignored/invalid/late overrides rather than silently
discarding them.

### P1: keep expensive diagnostics off the hot path

`BookManager::stats()` deliberately scans every local map to calculate current
probe layout. At production scale that reads tens of GiB, so it is a shutdown
or offline diagnostic only. If periodic metrics need probe data, maintain a
separate cheap historical sample or make the full scan opt-in.

### P1: add operation microbenchmarks

Measure add, partial/full execute, cancel, delete, replace-at-same-price,
replace-to-new-price, and last-level reclamation separately. Include probe and
cluster-length distributions; pool reuse and first-use paths have different
cache behavior.

### P2: strengthen residency and receive batching

Page touching establishes first-touch placement but does not prevent reclaim.
Evaluate strict `mlock`/`mlock2`, huge pages, and NUMA failure policy on the EC2
host. Separately, consider a batch-aware engine loop so `recvmmsg` batches are
not returned one saved datagram per virtual `next()` call.
