# AstraFeed design and acceptance plan

## Implemented decision

This branch replaces both the centered 65,536-level array and the branch-5
multi-level pooled radix design.  The implementation now uses:

1. a full-`uint32_t` price domain split into fixed page/level indices,
2. dense, stable aggregate pages allocated from a startup-sized resident arena,
3. one authoritative global order table, with no per-book hash table and no
   FIFO order links, and
4. fixed-depth bitmaps for best-price discovery.

The book mutation path does not resize, lock, perform a syscall, or probe an
unbounded chain. Capacity is selected at startup and is immutable while the
session is live. `md_engine` enables prefaulting by default; performance runs
must leave it enabled on the final NUMA node, while development runs may opt
into demand-paged mappings.

This branch was created from `main`, not from the tip of `5-to-add-numa`. The
review examined both trees; the architecture below describes the code now in
`OrderTable`, `PriceLevelStore`, `OrderBook`, and `BookManager`.

## Evidence from the full-day trace

`benchmarks/ItchTraceProfile.cpp` scans the raw length-prefixed ITCH file without
constructing books.  The local `01302019.NASDAQ_ITCH50` capture contains
368,366,634 records and 11,245,883,092 bytes.
The exact command, trace/profiler hashes, and complete stdout are retained in
[`trace-profile-01302019.txt`](trace-profile-01302019.txt).

Protocol widths, price limits, and lifecycle semantics in this review follow
the [official Nasdaq TotalView-ITCH 5.0 specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf).

| Measurement | Observed value | Design consequence |
|---|---:|---|
| Stock locates | 8,713 | All 65,536 descriptor slots exist at startup; only registered locates engage their slots. |
| Maximum locate | 8,713 | The capture is small, but the wire type still requires all 65,536 locate rows. |
| Add/replace prices | 191,919,099 | This is the population used for range analysis. |
| Prices outside the former first-price ±32,768 window | 123,424,129 (64.3%) | The centered array was not a viable representation. |
| Raw price range | 1 to 1,999,999,900 | Far prices are valid protocol values, not trace corruption. |
| Live-order high-water mark | 1,742,866 | Bounds active occupancy; the direct-index tier is instead admitted from the maximum order reference plus explicit headroom. |
| Maximum order reference | 329,176,641 | A direct fast tier is affordable, but a 64-bit fallback is required by the wire format. |
| Order references above `uint32_t` | 0 | Useful optimization evidence, not a correctness assumption. |
| Active side/price-level high-water mark | 707,130 | Aggregate state is much smaller than the total price domain. |
| Active `(stock_locate, page_index)` page high-water mark | 56,395 | Resident-page demand is measurable. |
| Distinct pages seen during the day | 68,941 | A monotonic 80,000-page test arena gives about 16% headroom. |
| Maximum pages for one symbol | 396 | Per-ticker hardcoded capacities are unnecessary. |
| Maximum observed level quantity | 4,822,255 | It fits today, but aggregate quantity must still be `uint64_t`. |
| Directory messages before/after `SS` | 8,713 / 1 | A post-`SS` directory message is real traffic. |
| Messages after system event `E` | 31,547 | `E` cannot terminate book processing. |
| Deletes after system event `E` | 31,546 | Cleanup mutations must remain enabled until system event `C`. |

The 64.3% range statistic compares each price with its symbol's first observed
price. It is a domain-pressure diagnostic, not the exact number the removed
centered book rejected, because that implementation could recenter after both
sides became empty. The implemented page/level store has no such moving
window.

The one post-`SS` `R` is an idempotent refresh of locate 1335, ticker `CFG-D`.
The implementation also supports a genuinely new late `R`; a single trace
cannot prove that it will never occur.

Build and run the reproducible trace profiler with:

```sh
cmake -S . -B build \
  -DASTRA_BUILD_APPS=OFF \
  -DASTRA_BUILD_TESTS=OFF \
  -DASTRA_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target astra_itch_trace_profile -j
build/benchmarks/astra_itch_trace_profile data/itch/unzipped/01302019.NASDAQ_ITCH50
```

## Findings that drove the redesign

### Why branch 5 reached the right domain but retained a costly latency shape

Branch 5 fixed several correctness issues worth preserving: it covers the full
`uint32_t` price domain, uses `uint64_t` aggregate quantity, preflights radix
creation, and preallocates shared pools. Its mutation path nevertheless contains
several dependent accesses that can amplify cache-cold latency:

1. linear-probe the per-book `LocalOrderRefMap`,
2. dereference the per-book `OrderArena`,
3. walk price root -> node -> node -> leaf,
4. translate a leaf handle through the shared pooled-level array, and
5. update FIFO links and, when a level empties, shared free arrays.

The radix stores only 256-way fragments, which is memory-efficient, but a found
level can require four dependent tree loads before the aggregate record is
known.  Pool reuse also moves unrelated symbols through the same cache/TLB
working set.  A low load factor improves average hash probing but does not bound
it, and cluster-shifting erase is still data-dependent.

This is a structural latency hypothesis, not a claim derived from the p50 alone.
The sampled benchmark publishes end-to-end mutation distributions, while a
separate untimed/profiled process records cycles and cache/TLB counters so
diagnostic instrumentation does not alter the accepted hot path. The redesign
keeps branch 5's correctness properties while replacing its dependent tree walk
with one root-to-page load and its per-book map with one global order lookup.

### The former price representation lost valid orders

The former `OrderBook` anchored 65,536 raw ticks on the first observed price.
With the ITCH scale of 10,000, that is only about -$3.2768 to +$3.2767 around the anchor.
An out-of-range add silently returns.  Replace is worse: it removes the old
order before it validates the new price and ID, so a rejected destination can
destroy valid state.

The implemented replacement covers the complete raw-price type. Replace now
reserves the new order slot and destination price page, validates arithmetic,
and only then changes the original logical state.

### Former aggregate arithmetic and mutation failures were unsafe

The removed `PriceLevel::total_qty` was `uint32_t`, although many 32-bit order
quantities can share a level. `PriceLevelState` now uses `uint64_t`, and public
top/book updates preserve that width. Over-cancel, duplicate ID, missing order,
exhausted capacity, arithmetic overflow/underflow, and invalid destination
return a compact `MutationResult`. Transactionally rejected input leaves the
book usable, while an internal invariant/address failure makes the local book
sticky-invalid. Every non-applied result is propagated by the production parser
and terminally invalidates that decoder channel instead of being silently
ignored.

### The former order index had data-dependent work

The removed per-book `OrderIdMap` performed linear probing, probed twice on
add, and cluster-shifted on erase. Both average and tail latency depend on key
distribution and churn.  The branch-5 local order map has the same fundamental
problem.  Nasdaq order references are day-unique, so one global authoritative
table also removes a redundant symbol-level lookup.

The removed order pool also carried `prev/next` links and price levels carried
`head/tail`. ITCH execute, cancel, delete, and replace identify the order
reference directly, so the implemented aggregate L2 book has neither FIFO
links nor per-book order maps.

### Parser and lifecycle behavior was inconsistent

The build compiled `src/replay/itch/ItchParser.cpp`, while the decoder included
the nominally canonical `include/astra/parser/ItchParser.hpp`; a second,
different implementation existed under `src/parser`.  This branch now builds
one canonical parser and removes the duplicates.

System-event code `E` means End of System Hours, while system-event code `C`
means End of Messages.  They require independent phases.  Order-message type
`E` (Order Executed) and type `C` (Order Executed With Price) also now have
independent wire handlers that share only the quantity-decrement primitive.
Broken Trade `B` has no current-book effect in a book-only engine and must not
call `reverseExecution`.

At `SS` (an ITCH `S` message with event code `S`), all known books are
prepared. A later Stock Directory `R` follows this policy:

Backing-store capacities and 65,536 optional `OrderBook` descriptor slots were
already allocated when `BookManager` was constructed. `SS` completes bulk
preparation of the books known at that point; it does not seal stock-directory
membership. The full locate root, a descriptor slot, and configured page/order
capacity remain available to a late administrative `R` without another heap
allocation.

1. Same locate and ticker: refresh metadata in place and preserve the book.
2. New locate: claim already-resident resources on the administrative path.
3. Same locate with a different ticker: reject and invalidate; never retarget a
   live book.
4. `A/F/E/C/X/D/U` never allocate a book.

System-event `M` only moves the lifecycle to `PostMarketHours`; it does not
disable order mutations. This is distinct from an `S` message whose event code
is `E` (End of System Hours), which moves to `PostSystemHours` without changing
book state. The parser continues to accept every `A/F/E/C/X/D/U` mutation in
that phase, including message type `E` (Order Executed) and message type `C`
(Order Executed With Price), so cleanup can finish. `SC` (the `S` message with
event code `C`, End of Messages) is terminal: subsequent ITCH book, directory,
and administrative messages are rejected. The parser does not perform an
in-process book reset; the production contract ends that process/session there.

### Packet commit hardening status

The decoder now validates complete Mold framing and every exact ITCH message
length in a first pass before applying the first mutation from a datagram. It
uses a constexpr 256-entry message-length table:

```text
S12 R39 H25 Y20 L26 V35 W12 K28 J35 h21
A36 F40 E31 C36 X23 D19 U35 P44 Q40 B19 I50 N20
```

Parser/book failures now propagate as `InvalidItchMessage`; session bytes are
compared after the first packet; an ahead heartbeat marks a gap; and sequence
addition is overflow-checked. A Mold session mismatch invalidates the channel
and requires a fresh process rather than generation-tagging only part of the
state.

The batched UDP path now drops a whole datagram when `MSG_TRUNC` is set or the
reported size exceeds its buffer, matching the scalar receiver rather than
forwarding a clamped prefix. Kernel, replay, DPDK, and gap storage now share
the 2,048-byte packet limit. The remaining gap-buffer replacement and
multi-interface kernel-receiver policy are transport follow-on work, not
properties of the book data structure.

### Other project-wide deterministic-latency risks

- `GapBuffer` is a 1,048,576-slot hash table of roughly 2 GiB. Insert/find/erase
  may probe all 1,048,576 slots, and cluster-copy erase may scan another
  1,048,575 slots. The bound is finite but far too large and data-dependent for
  a deterministic recovery path. Index a fixed recovery ring directly by
  sequence number and store the full sequence/generation in each slot.
- `md_engine` now exits when explicitly requested CPU affinity cannot be
  established. Standalone benchmark binaries still require external pinning,
  for example `numactl --physcpubind=<cpu> --membind=<node>`.
- Kernel multicast uses `INADDR_ANY`; redundant-feed deployments on multiple
  ENIs need an explicit interface address/index.
- DPDK initializes EAL before book construction, reapplies the requested CPU
  after EAL, and validates fixed-offset frames before falling back to the
  general VLAN/IPv4/UDP parser even when hardware flow filtering is off.
  Descriptor, mempool, burst, port, and queue settings are validated and
  reported. Effective kernel socket buffer and busy-poll settings still need
  equivalent reporting on the ordinary UDP path.
- `ASTRA_BUILD_BENCHMARKS` now builds the trace profiler, the synthetic
  order-book microbenchmark, and the full parser/book replay benchmark. The
  benchmark boundary and configuration must still be recorded with every
  published result.
- Timing now compiles portably. x86/x86_64 uses RDTSCP; other architectures use
  the monotonic-clock fallback and can run correctness tests. Only the fixed
  x86 EC2 setup is authoritative for the nanosecond acceptance gate.

## Implemented hot-path architecture

```text
ITCH message
    |
    +--> direct locate descriptor (65,536 slots)
    |
    +--> global order table ------------------------------------------+
    |       order_ref -> {locate, side, page handle/index, level, qty}|
    |                                                               |
    +--> book root[price.page_index] -> resident page <-+
                                      |
                                      +--> slot[price.level_index]
                                      +--> fixed-depth occupancy summaries
                                      +--> cached best bid / best ask
```

### Price pages

`PriceLevelStore` reserves one flat `65,536 x 65,536` root of 32-bit page
handles. Its exact slot is
`(static_cast<uint64_t>(stock_locate) << 16) | page_index`. The 16 GiB mapping
contains every 16-bit locate row, although locate zero remains reserved and is
rejected. A genuinely new late `R` therefore needs no new root allocation. An
`R`/`SS` preparation marks its row usable; with prefaulting enabled the entire
root is resident before the feed, while demand-paged development runs fault
root pages on first touch. Split raw price as:

```cpp
page_index  = price >> 16;
level_index = price & 0xffff;
```

These names always mean address components, not configured price-window
boundaries. For example, raw `100000` becomes `page_index=1` and
`level_index=34464`, and `(1 << 16) | 34464` reconstructs `100000`. The same
split covers both `0` and `UINT32_MAX` without recentering.

A page contains separate dense arrays for the two sides:

```cpp
struct alignas(16) PriceLevelState {
  uint64_t total_qty;
  uint32_t order_count;
  uint32_t reserved;
};

struct PricePage {
  PriceLevelState bids[1u << 16]; // 1 MiB
  PriceLevelState asks[1u << 16]; // 1 MiB
};

static_assert(sizeof(PriceLevelState) == 16);
static_assert(sizeof(PricePage) == 2u * 1024u * 1024u);
```

That is exactly one 2 MiB page. Handle zero is invalid; committed handle `h`
selects `pages[h - 1]`, while its owner record stores the corresponding
`stock_locate` and `page_index`. A mutation already knows its side, so a fetched
cache line contains four adjacent same-side price levels rather than two prices
interleaved with unused opposite-side data.  No level straddles a cache line.

Each side has a level-index occupancy bitmap, and each book has the same
hierarchy over occupied page indices. The hot records are not attached to
those large bitmaps: an 8-byte `PageOwner` arena remains compact for partial
`E/X`, a 16-byte `PageSummary` contains both side summaries, and a 32-byte
`BookSummary` contains both book-side summaries. The 8,384-byte occupancy
records live in separate fixed arenas. Four page summaries or two book
summaries therefore tile one cache line instead of making each best/count read
stride through an 8,448-byte record.

A singleton page caches its sole `level_index` and keeps its level bitmap
empty. The `1 -> 2` transition materializes both bits, and `2 -> 1` collapses
the bitmap again. Final singleton removal consequently performs zero page-level
bitmap touches. Multi-level best removal uses `ctz/clz` at a fixed hierarchy
depth; it never scans an arbitrary run of empty prices. Cached best bid/ask
handles keep the common read path bitmap-free. Top-ten output stops after the
tenth stored level, so one full side performs at most nine `nextWorse()` calls
and 18 fixed-depth bitmap searches instead of searching for an unused eleventh
level.

Pages come from one fixed anonymous mapping aligned to 2 MiB. Anonymous memory
provides zero-filled storage. On Linux, a named hot mapping requires build
headers and runtime policy supporting `MADV_HUGEPAGE` and anonymous-VMA names;
construction fails before feed processing if either request is unavailable or
denied. Acceptance still treats the advice as a hint, not residency proof. With prefaulting enabled,
the arena and its side metadata are touched before message processing. First
use of a `(locate, page_index)` pair
then claims a resident handle with a bounded monotonic commit; it performs no
heap allocation, clearing pass, or syscall. Handles never move and pages are
not recycled during a live session. Exhaustion returns
`PricePageCapacityExceeded`, not a fallback allocation. Without prefaulting,
first touch may fault and is intentionally excluded from deterministic
performance acceptance.

The hint is not acceptance evidence. The replay declares
`hot_arena_schema=redesign_v1` and reports the exact base and mapped extent for
all eleven order, descriptor, and price-store arenas. The harness captures full
`/proc/PID/smaps` and `/proc/PID/numa_maps`; every declared arena must be one
exact 2 MiB-aligned, correctly named VMA, fully reported as `AnonHugePages`,
and wholly resident on the requested NUMA node. Missing and unknown schemas
fail closed.

Every nonzero root value is validated against the committed-page bound and its
owner's locate/page-index pair before it is accepted. A cross-book alias,
same-owner/wrong-page alias, or out-of-range handle returns
`InvariantViolation` on reservation; read traversal sanitizes it before page
metadata indexing. These checks do not add work to existing-order partial
execute/cancel paths, which use the already validated address in `OrderState`.

### Global order table

The direct tier reserves one slot for every reference below its configured
limit; a slot represents a live order only while its active flag is set:

```cpp
struct alignas(16) OrderState {
  uint32_t qty;
  uint32_t price_page_handle;
  uint16_t price_level_index;
  uint16_t locate;
  uint8_t side;
  uint8_t flags;
  uint16_t price_page_index;
};

static_assert(sizeof(OrderState) == 16);
static_assert(alignof(OrderState) == 16);
static_assert(64 % sizeof(OrderState) == 0);
```

The direct table starts at a cache-line/page-aligned address, so four records
tile each 64-byte line and no record straddles two lines.  Do not align every
record to 64 bytes: that would increase a `2^29` table from 8 GiB to 32 GiB.
The stored page handle and `level_index` let existing-order mutations bypass the
`(stock_locate, page_index)` root lookup. The stored `price_page_index` is the
expected logical owner of that handle; comparing it with the compact page-owner
record detects both cross-book substitution and same-book/wrong-page
substitution without restoring the root lookup.

Production uses one process per Mold session.  The direct table is a new zeroed
mapping for that process, so `OrderState` needs no generation field.  A session
change invalidates the channel and requires a fresh process because the price
pages, bitmaps, directory, sequencing, and fallback table all require a
coherent reset together.

The implemented hybrid exploits the available memory without narrowing the
wire contract:

- A startup-sized direct tier handles `order_ref < direct_limit` with one
  indexed access.  A `2^29` limit costs 8 GiB and covers the observed maximum
  with about 63% ID headroom.
- A fixed-capacity set-associative fallback stores arbitrary 64-bit references.
  Each key has one or two distinct candidate buckets, and each bucket has two
  32-byte ways. A lookup therefore examines at most four entries. Reservation
  scans every candidate for a duplicate before selecting an empty way. There is
  no linear probing or cluster shifting.
- Direct-slot count may be any nonzero value that fits `size_t`; fallback bucket
  count must be a nonzero power of two. Exhaustion returns
  `OrderTableCapacityExceeded` without partially applying the order mutation.

Add reserves an order record and price address before updating the aggregate
slot. `OrderTable::findState()` is the mutation lookup: on GCC/Clang its direct
branch is forced inline and performs the capacity comparison, indexed state
load, and active-bit test in the caller; only the bounded fallback scan is out
of line. Execute/cancel/delete find the record once and use its stored page
handle plus `level_index`, so they perform no price-root lookup. Partial `E`
and `X` mutations call `PriceLevelStore::reduceChecked()`, which fuses resident
page-owner validation, level invariant checks, and the aggregate decrement;
the order quantity is stored only after that succeeds. Replace preflights the
new ID and destination page before changing the original. There is no per-book
order map and no order FIFO.

### Pinned acceptance-fixture budget for the 250 GB test instance

`estimateBookStorageFootprint()` calculates page-rounded mappings before any
arena is constructed. For a 4 KiB Linux system page and the current 64-byte
`std::optional<OrderBook>` descriptor-slot ABI, the pinned 2019-01-30 plan is:

| Component | Pinned 2019 acceptance capacity | Exact mapped/planned bytes |
|---|---:|---:|
| Direct order tier | `2^29` x 16 bytes | 8,589,934,592 |
| Fixed fallback table | `2^20` x 64-byte buckets | 67,108,864 |
| Flat `(stock_locate, page_index)` root | `2^32` x 4 bytes | 17,179,869,184 |
| Prepared-locate flags | 65,536 x 1 byte, 2 MiB-extent-rounded | 2,097,152 |
| Side-separated aggregate pages | 80,000 x 2 MiB | 167,772,160,000 |
| Page-owner records | 80,001 x 8 bytes, 2 MiB-extent-rounded | 2,097,152 |
| Compact per-page summaries | 80,001 x 16 bytes, 2 MiB-extent-rounded | 2,097,152 |
| Per-page, per-side occupancy bitmaps | 160,002 x 8,384 bytes, 2 MiB-extent-rounded | 1,342,177,280 |
| Compact per-locate summaries | 65,536 x 32 bytes | 2,097,152 |
| Per-locate, per-side occupancy bitmaps | 131,072 x 8,384 bytes | 1,098,907,648 |
| **Mapped-array subtotal** |  | **196,058,546,176** |
| Preallocated optional book descriptors | 65,536 x 64 bytes on this ABI | 4,194,304 |
| **Planned book storage** |  | **196,062,740,480 bytes (182.598 GiB)** |

The 68,941 pages ever seen in the measured trace would occupy 134.65 GiB; the
80,000-page mapping retains roughly 16% page-count headroom. The plan counts
all eleven final mapped extents and reports the logical descriptor payload
separately; it excludes allocator/kernel metadata, the replay file/message
buffers, the resident timed-sample vector, transport, gap recovery, process
code, and the OS. System-page size and descriptor ABI can also change the exact
total, so the target binary's `--storage-plan-only` output is authoritative.

A nominal 250 GB host exposes about 232.8 GiB in total, but the acceptance
command uses `numactl --membind=<one-node>`. The selected node's `MemTotal` and
`MemFree`, plus cgroup headroom, must each cover the derived plan and the
default 16 GiB reserve: 213,242,609,664 bytes (198.598 GiB) for the plan above.
A multi-node host can therefore have enough aggregate RAM and still be
ineligible because no single node is large enough. Use a topology with one
qualifying node or revise and reapprove the binding/admission policy; do not
silently let an accepted run spill across nodes.

Memory capacity values are runtime configuration validated at startup and then
frozen. Price split, slot layout, alignment, at-most-two-bucket/two-way fallback
shape, message lengths, and wire field widths remain compile-time invariants.

## Runtime configuration

`md_engine` deliberately has no production capacity default. Startup requires
`ASTRA_BOOK_CAPACITY_PROFILE`. The exact
`nasdaq-itch-20190130-acceptance-v1` value selects
`BookManagerConfig::pinnedNasdaqItch20190130Acceptance()` and rejects capacity
or evidence overrides. It preserves the pinned 80,000-page acceptance fixture
without presenting it as a safe deployment default.

Every other profile name is a custom deployment and requires all of
`ASTRA_BOOK_CAPACITY_EVIDENCE_FILE`,
`ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256`, `ASTRA_ORDER_DIRECT_SLOTS`,
`ASTRA_ORDER_FALLBACK_BUCKETS`, `ASTRA_PRICE_PAGE_CAPACITY`,
`ASTRA_PROFILED_MAX_ORDER_REF`, `ASTRA_PROFILED_UNIQUE_PRICE_PAGES`,
`ASTRA_MIN_DIRECT_ORDER_HEADROOM`, and
`ASTRA_MIN_PRICE_PAGE_HEADROOM`. The expected manifest hash must be exactly 64
lowercase hex characters. The engine computes SHA-256 over the exact bytes it
parses and rejects a mismatch. Direct and price reserves are positive absolute integers, so
startup admission has no floating-point or rounding ambiguity. The profiled
maximum reference must remain in the direct tier; the profiled price value is
the lifetime count of unique `(stock_locate, page_index)` pairs because page
handles are monotonic and never reclaimed. The fallback bucket count is still
explicit, but aggregate occupancy cannot guarantee arbitrary hash collisions:
an aliased one-bucket key can fail on its third contender, while a two-bucket
key can fail on its fifth. Both fail loudly with bounded work.

`ASTRA_BOOK_PREFAULT` defaults to `on` for custom profiles and may explicitly
be set `off` for development. The pinned acceptance fixture requires it on.
Before mapping any arena, the engine prints `book_capacity_profile` with the
evidence hash and minimum/effective headroom, followed by `book_storage_plan`.
`md_engine --book-storage-plan-only` performs those checks and exits before
mapping. The evidence file is canonical ASCII with one final LF and exactly
these ordered keys:

```text
schema=astra_book_capacity_evidence_v1
profile_name=<audit-safe profile token>
corpus_manifest_sha256=<64 lowercase hex>
profiler_sha256=<64 lowercase hex>
order_direct_slots=<canonical positive integer>
order_fallback_buckets=<canonical positive integer>
price_page_capacity=<canonical positive integer>
profiled_max_order_ref=<canonical positive integer>
profiled_unique_price_pages=<canonical positive integer>
minimum_direct_order_headroom=<canonical positive integer>
minimum_price_page_headroom=<canonical positive integer>
```

Extra/reordered fields, CRLF, leading-zero integers, and non-ASCII input fail
closed. Parsed values are authoritative and every duplicated environment value
must match, binding startup to the approved multi-day evidence rather than an
unverified hash-shaped label.

The replay binary takes the same three book capacities at runtime through
`--direct-order-slots=N`, `--fallback-buckets=N`, and
`--price-page-capacity=N`; their defaults are `2^29`, `2^20`, and `80000`.
These defaults are deliberately the pinned 2019 acceptance fixture, not a
live-deployment sizing policy. Unlike `md_engine`, replay book prefaulting is
opt-in through `--prefault`.
Custom acceptance overrides also require `--capacity-profile-name`,
`--capacity-evidence-file`, and `--capacity-evidence-sha256`. The replay uses
the same in-process loader and repeats the computed identity in its storage
plan, ready marker, and final record. Unbound overrides are development-only
and cannot pass the live harness.
`--sample-capacity=N` separately sets the resident timed-sample capacity
(default `8,388,608`, and never below `--min-samples`). The benchmark resizes,
touches, and clears that vector before replay, independent of `--prefault`, so
timed collection cannot grow or first-touch its storage.

`--storage-plan-only` validates the runtime book capacities, prints the
`redesign_v1` schema, `system_page_bytes`, every arena's mapped extent, the
legacy mapped-array subtotal, logical descriptor bytes, their exact
`planned_storage_bytes`, the three effective capacities, and prefault mode,
then exits before constructing the stores or allocating replay buffers. It is
therefore an exact book-storage admission plan for that binary, not a
whole-process RSS estimate. During a real replay,
`--require-zero-post-warmup-faults` fails if either the `getrusage` minor- or
major-fault delta after the configured book-message warmup is nonzero.

The replay flushes an `itch_book_replay_ready` record only after construction,
the configured book/sample-storage prefault, and trace consumption through
System Event `S` have completed. A book mutation before that boundary is an
error. Ready and final include exact `prelude_records` and `prelude_bytes`, the
schema and every declared arena's exact VMA base and mapped extent, the sample
interval/warmup, and the versioned algorithm-and-seed identity
`fixed_block_offset_v1_splitmix64_seed_61737472612d6974`. The final replay
record repeats that identity. The acceptance monitor waits for the ready marker
before capturing full `smaps`, `smaps_rollup`, and `numa_maps`. The benchmark
remains blocked at a start gate until those artifacts and the configured sample
schedule pass validation; only then does the monitor release timed replay. It
does not infer completion from total RSS or treat THP policy as actual backing.

## Live A/B replay and transport measurement

The live topology has one `md_engine` process and one synchronized sender
process. The sender reads one length-prefixed ITCH stream, sends each logical
MoldUDP64 packet to line A, publishes the same buffer to the dedicated line-B
thread, and waits for B to finish before advancing. The default B target is
1,000 ns after A. Running two independent sender processes is not equivalent:
they would not share sequence, pacing, or completion state.

`scripts/run_sender.sh` is the sole sender entry point. It begins with 100 Mold
heartbeats at 10 ms intervals by default. Every heartbeat advertises next
sequence 1 without advancing the source and contributes no latency sample.
This preamble warms route/neighbor state and binds the receiver to the Mold
session before sequenced traffic.

The sender supports three pre-market policies:

- `ASTRA_PREMARKET_REPLAY_MODE=off` uses the configured flat packet rate,
  apart from an optional pause immediately after `SS`.
- `ASTRA_PREMARKET_REPLAY_MODE=timestamp` follows ITCH timestamps between
  `SS` and `SQ`, divided by `ASTRA_PREMARKET_SPEEDUP`. The pinned trace spans
  about 5.5 hours in that interval, so speedups 10, 33, and 165 take roughly
  33, 10, and 2 minutes.
- `ASTRA_PREMARKET_SECONDS=N` spreads the `SS`-to-`SQ` segment evenly over
  `N` seconds for a smoother deterministic stress window.

Use `scripts/run_sender.sh --help` for the positional interface and all
environment controls. The operational DPDK ENI/VFIO/hugepage procedure is kept
separately in
[`dpdk-aws-ec2-setup.md`](dpdk-aws-ec2-setup.md).

The ordinary UDP runner supports scalar `recv` and Linux
`recvmmsg`/`batch` modes. `ASTRA_UDP_DROP_METRICS=on` enables kernel overflow
reporting. A clean completed run requires sender completion and end-marker
success, matching sender/receiver next sequence, `channel_status_name=Good`,
the exact trace latency count, and zero malformed/drop/error counters.

The decoder accepts a Mold end marker only after exact sequence recovery and
terminal ITCH system event `C`. The first valid marker from either redundant
line stops the engine; it intentionally does not drain late duplicate tail
frames, so receiver A/B packet totals can differ slightly even when the entire
logical stream completed.

In `ASTRA_DPDK_LATENCY_MODE=packet`, timing begins after
`rte_eth_rx_burst()` returns. It covers frame parsing, Mold validation and
sequencing, ITCH dispatch, and book mutation. The elapsed packet processing
time is divided by the number of newly processed messages and entered with
that weight. The reported distribution is therefore amortized CPU processing
nanoseconds per logical message. It excludes NIC, network, RX-queue, and
sender-to-book latency; duplicate A/B packets, heartbeats, and the end marker
produce no latency samples.

For the three recorded 2026-07-23 load points, every run reached sequence
368,366,635 with 368,366,634 latency samples, 8,713 symbols, a healthy channel,
and zero malformed, missed, error, or no-buffer counts. Packet-path evidence
was:

| Configured rate per line (packet/s) | Line A packets | Line B packets | Fast path | Fallback/filtered |
| ---: | ---: | ---: | ---: | ---: |
| 100,000 | 18,418,433 | 18,418,432 | 36,836,865 | 6 |
| 150,000 | 18,418,433 | 18,418,432 | 36,836,865 | 5 |
| 200,000 | 18,418,433 | 18,418,431 | 36,836,864 | 6 |

The central distribution improved as configured rate increased, but the deep
tails rose: from 150,000 to 200,000 packet/s per line, mean through p99
improved by 0.45% to 1.29%, while p99.9 and p99.99 worsened by 4.27% and
4.35%. These are single-run observations rather than deterministic acceptance
evidence. Repeat fresh processes, retain every sender and engine log (including
elapsed time, send failures, and `line_b_delay_overruns`), and compare the
worst repeated tail rather than averaging percentiles.

## Reproducible measurement commands

Build release binaries and tests:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run the synthetic mutation benchmark. Repeat the named gate option to enforce
separate ceilings on the distributions that represent the accepted workload:

```sh
build/benchmarks/astra_order_book_benchmark \
  --iterations=100000 \
  --gate=direct_partial_execute:150:300:600 \
  --gate=remove_cross_page_best:150:300:600
```

The ten reported workloads are `direct_partial_execute`,
`direct_partial_execute_wide_pages`, `direct_partial_cancel_wide_pages`,
`add_existing_level`, `delete_populated_level`, `replace_cross_page`,
`remove_current_best`, `remove_cross_page_best`, `fallback_partial_execute`,
and `fallback_four_slot_miss`. The two wide-page partial workloads keep one
order in each of up to 4,096 pages and visit them in a fixed-seed permutation,
exposing regressions in the compact `OrderState -> PageOwner -> PriceLevel`
path used by partial ITCH `E` and `X` without adding bitmap work.
`remove_cross_page_best` keeps one bid level in
each of up to 4,096 pages and allocates handles in a fixed-seed permutation,
making every timed deletion empty a singleton page, skip its level bitmap, and
search the book-level bitmap for the next page without a price/handle-order
locality shortcut. The last case uses bounded untimed
setup to fill the two candidate buckets and then measures a missing reference
that examines all four fallback slots. The `fallback_setup` line reports actual successful
`primary_lookups` and `secondary_lookups`, with counts for primary way 0/1 and
secondary way 0/1. Each workload reports p50, p90, p99, p99.9, and maximum.
`--gate=WORKLOAD:P50:P99:P99.9` accepts any workload name in the list above.
Every named gate requires three positive monotonic nanosecond ceilings, and a
workload can be gated only once. Unknown names, duplicate workloads, malformed
specifications, zero thresholds, and inverted thresholds are rejected before
measurement. The numbers in the example are illustrative; replace them with
the approved controlled-host ceilings. Any requested gate fails closed on a
non-x86 host because portable-clock measurements are not equivalent to the
x86 RDTSCP acceptance clock. The legacy `--max-direct-p50-ns`,
`--max-direct-p99-ns`, and `--max-direct-p99-9-ns` flags still target
`direct_partial_execute`; zero disables an individual legacy threshold, and
they cannot be combined with a named gate for the same workload.

Run the full length-prefixed ITCH file through the parser and production book.
The example CPU and node must be replaced with an isolated CPU and its local
NUMA node on the selected instance:

```sh
numactl --physcpubind=2 --membind=0 \
  build/benchmarks/astra_itch_book_replay_benchmark \
  data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  --prefault \
  --sample-every=64 \
  --warmup-book-messages=1000000 \
  --min-samples=1000 \
  --max-p50-ns=150
```

The replay times sampled parser/book calls, not file reads. It invokes
`ItchParser::handleMessage()`, including empty/type/exact-length validation.
The live Mold decoder instead performs one packet-wide framing/type/length
prepass and invokes `handlePrevalidatedMessage()`, avoiding a repeated
length-table lookup. Both entrypoints converge at the audited
`dispatchMessage()` and A/F/E/C/X/D/U mutation closure. Replay is consequently
slightly conservative relative to live per-message dispatch, while excluding
Mold framing, sequencing, UDP, and file I/O from the timed interval.

The sampler selects one fixed-seed pseudo-random offset within every complete
`--sample-every` block of book mutations, preserving the approximate rate
without a fixed periodic message-type bias. The ready and result records publish
the exact versioned
schedule ID `fixed_block_offset_v1_splitmix64_seed_61737472612d6974`; the
ready record also publishes the interval and warmup, allowing validation before
timing begins. `--min-samples` defaults to `1000`, applies to the aggregate
sample array, and rejects a run that did not collect enough evidence. For an
acceptance run, add `--expect-records=<exact-records>` and
`--expect-bytes=<exact-bytes>` from `astra_itch_trace_profile`; a mismatch is a
hard failure. The replay also fails on a parser error, page-capacity failure,
nonzero final live orders, an ending phase other than End of Messages, or an
enabled p50/p99/p99.9 threshold failure. Add `--max-p99-ns=<limit>` and
`--max-p99-9-ns=<limit>` using approved absolute branch-6 ceilings.

The aggregate `itch_book_replay` line reports `sample_count`, the sample policy
and warmup, prefault mode, configured direct-slot/fallback-bucket/price-page
capacities, planned/effective storage bytes, resident sample capacity,
post-warmup minor/major faults, committed pages and capacity failures,
records/bytes, final state, and RDTSC calibration (`rdtsc_overhead_ticks`,
`rdtsc_ticks_per_second`, and `now_ns_overhead_ns`) alongside its distribution.
`--storage-plan-only` reports the allocation plan without constructing it, and
`--require-zero-post-warmup-faults` makes either fault count fatal. Seven
`itch_book_replay_type` lines report sample count and p50/p90/p99/p99.9/maximum
independently for `A/F/E/C/X/D/U`; a type with no selected sample is explicitly
marked `distribution=unavailable`.

Run correctness observation separately from latency acceptance. The existing
physical digest hashes every applied mutation, its raw message, live count/top,
affected order, lookup path, price-page handle, and resolved level aggregate.
It is intentionally strict evidence for repeatability of the same binary and
layout. Because those reads alter cache state, do not enable them in a
published timing run:

```sh
build/benchmarks/astra_itch_book_replay_benchmark \
  data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  --sample-every=1024 \
  --min-samples=1000 \
  --expect-records=<exact-records> \
  --expect-bytes=<exact-bytes> \
  --mutation-digest
# Repeat on the same trace with
# --expect-mutation-digest=<physical value printed above>
# --expect-semantic-mutation-digest=<semantic value printed above>.
```

The CLI rejects mutation-digest observation combined with any nonzero latency
threshold. The same correctness run also computes schema
`applied_itch_book_semantics_v1_fnv1a64le`: it hashes every applied raw ITCH
book message and observable logical post-state, including book live count/top,
affected orders, and affected resting-price aggregates. It excludes lookup
paths, page handles, and addresses, so `--expect-semantic-mutation-digest`
provides an explicit equivalence gate across physical-layout changes.
`state_checksum` is retained only as a compact diagnostic; accepted correctness
evidence uses exact record/byte gates and repeated matching physical and
semantic digests.

A separate untimed capacity profile remains available through
`astra_itch_trace_profile`.

On x86/x86_64, timing uses RDTSCP. Other architectures use the monotonic-clock
fallback and can execute correctness tests, but any enabled command-line
latency gate fails closed there because portable-clock measurements are not
equivalent acceptance evidence.

## Implementation status

Completed in this branch:

1. Canonical parser consolidation, all 65,536 locate slots, late/repeated `R`
   handling through startup-preallocated descriptors after `SS`, independent
   system-event/order-message `E/C` behavior, and terminal `SC` enforcement.
2. `PriceLevelStore`, side-separated 16-byte levels, full-domain roots,
   monotonic page reservation, cached best prices, and bounded hierarchical
   bitmap search.
3. One authoritative `OrderTable` with an indexed direct tier and a fixed
   at-most-two-bucket/two-way 64-bit fallback. Per-book hashes and FIFO links
   are gone.
4. Transactional add/replace resource reservation, typed mutation results,
   64-bit aggregate quantities, explicit price-zero validity, and session
   mismatch invalidation for the one-process-per-Mold-session contract.
5. Boundary, capacity, hierarchy, transactional, lifecycle, and fixed-seed
   oracle tests, plus synthetic and full-trace benchmark binaries.

Follow-on transport/sequencing work from the broader audit remains separate
from this book redesign, including replacing gap-buffer hashing and validating
the Linux `recvmmsg`/NUMA deployment before final production rollout.

### Local verification recorded on 2026-07-22

The current source built successfully in a `Release` configuration
(`-O3 -DNDEBUG`) on macOS ARM64. All 197 Release CTest entries are accounted
for: the 192 non-socket entries passed together, and all five `UdpReceiverTest`
cases passed with loopback access. ASan and UBSan each passed 185 non-receiver
GoogleTest entries plus the five receiver cases in a direct rerun.

The final Release replay binary, SHA-256
`9ab0068a345e51485110ee91a1ce56101dd91291de8dde32f4d8ed9f5175af80`,
passed the complete `01302019.NASDAQ_ITCH50` correctness replay against exact
gates for 368,366,634 records and 11,245,883,092 bytes. It applied 363,118,215
book mutations, committed 68,941 price pages with zero capacity failures,
finished with zero live orders in End of Messages, and matched physical digest
`11602566873588607264` and semantic digest `13876319090171585636`. That local
run was deliberately non-prefaulted and used the portable clock, so it proves
correctness rather than the latency target. Controlled prefaulted x86 EC2/NUMA
digest and latency processes remain required for sign-off.
The exact binary/trace hashes, command arguments, invariant fields, and
consolidated two-process result are retained in
[`full-trace-replay-verification-20260722.txt`](full-trace-replay-verification-20260722.txt).

The version-2 Release disassembly verifier passed the final redesign binary
(SHA-256 above) with 94 selected functions. It starts from both replay and live
parser entrypoints, dispatch, and the book mutation roots, then recursively
includes resolved direct in-binary calls and tail calls. Allocator, mapping,
syscall, lock, lock-prefix, and indirect-call targets are forbidden; only the
named cold `fail()` and `applyBookFailure()` formatting boundaries may allocate.
Standard-library/ABI/PLT bodies, symbol resolution, and those two named cold
helpers remain explicit static-analysis trust boundaries.

Portable-clock local benchmark runs completed, but their nanosecond values are
not acceptance evidence and are intentionally not treated as proof of the
150 ns target. Only a controlled x86 EC2/NUMA branch-6 run can establish that
result.

## Acceptance gates

- Full replay must match the exact record and byte counts, collect the required
  samples, report no parser or mutation failure, exhaust no capacity, finish
  with zero live orders, and reach End of Messages. Physical and semantic
  mutation digests run in separate processes from latency measurement.
- Lifecycle coverage includes repeated and new `R` after `SS`, directory
  traffic after system-event `E`, order-message `E` and `C` as independent
  mutations, cleanup through Post System Hours, and rejection of every message
  after terminal system-event `C`.
- Direct order lookup examines one slot; fallback examines at most two two-way
  buckets. Existing-order `E/C/X/D` mutations need no price-root lookup.
  Bitmap predecessor/successor search and top-ten traversal retain their tested
  finite bounds.
- After full prefaulting, the disassembly verifier must cover both parser
  entrypoints, dispatch, every `A/F/E/C/X/D/U` handler, and the transitive
  in-binary mutation closure. Successful hot paths may not call allocators,
  mapping/syscall, lock, or indirect targets. Post-warmup minor and major fault
  counts must remain zero.
- Run at least five fresh branch-6 processes with the same EC2 instance, isolated
  core, NUMA node, compiler, build flags, clock policy, huge-page policy, trace,
  warmup, and fixed sampling schedule. Every aggregate p50 must be at most
  150 ns. Aggregate and per-message-type p99/p99.9 must meet explicitly
  approved absolute ceilings; p90 and maximum remain reported diagnostics.
- Retain all aggregate and `A/F/E/C/X/D/U` distributions, sample counts,
  record/byte gates, sampling identity, capacity configuration, page usage,
  clock calibration, digests from correctness processes, and separate
  hardware-counter evidence.

Run the gate with `scripts/run_order_book_acceptance.sh`, supplying the exact
trace counts, isolated CPU/local NUMA node, and all three absolute latency
ceilings. It evaluates only the supplied branch-6 binary, runs a minimum of five
latency processes, and retains every result plus the worst distribution. With
`--correctness-digest`, it performs digest discovery and verification in
separate processes.

The harness exports the verified clean commit, performs a clean Release rebuild
of the replay target, and requires the supplied, rebuilt, and retained binaries
to be byte-identical. It derives the storage plan from
`--storage-plan-only`; verifies node and cgroup headroom, CPU isolation,
performance governor, swap, THP policy, VMA identity, residency, and NUMA
placement; and releases each replay start gate only after ready-marker memory
evidence passes. The trace and all relevant inputs are hashed before and after
the run. The final `manifest.sha256` covers every retained artifact.

Branch 6 never seals the stock-directory universe at `SS`. Its 65,536
descriptor slots and backing arenas already exist, so late/repeated `R`
messages remain an administrative path without hot-path growth. System event
`E` moves lifecycle phase but does not stop `A/F/E/C/X/D/U`; only system
event `C` is terminal.

Recommended hardware-counter capture:

```sh
perf stat -e cycles,instructions,branches,branch-misses,cache-misses,\
dTLB-loads,dTLB-load-misses,page-faults,context-switches,cpu-migrations \
  numactl --physcpubind=2 --membind=0 \
  build/benchmarks/astra_order_book_benchmark --iterations=100000
```

The `--max-*-p50-ns` options are convenience gates, not substitutes for the
full correctness and tail-latency review above. Generic CI gates state and
bounded work only; the 150 ns gate belongs to the fixed x86 EC2 environment.
