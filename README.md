# astra-feed-engine

AstraFeed is a low-latency C++ market data feed handler for MoldUDP64-wrapped
NASDAQ ITCH 5.0 replay. The primary benchmark setup uses redundant A/B UDP
sender lines feeding one `md_engine` receiver.

## Order-book architecture

The order book is process-wide and single-writer:

- `OrderTable` uses the ITCH order reference directly for the configured dense
  tier. References beyond that tier use two deterministic hash buckets with two
  ways each, so a fallback lookup examines at most four slots. There is no
  linear probing or cluster-shifting erase.
- `PriceLevelStore` covers the complete 32-bit ITCH price domain. It always
  names the two address components `page_index = raw_price >> 16` and
  `level_index = raw_price & 0xffff`. The root slot is
  `(stock_locate << 16) | page_index`; its 32-bit handle selects the resident
  page containing `level_index`.
- Each 2 MiB price page contains separate 1 MiB bid and ask arrays. Every level
  stores a 64-bit aggregate quantity and a 32-bit order count.
- Existing-order mutations carry the page handle and `level_index` in their
  16-byte `OrderState`, so execute, cancel, and delete do not repeat the price
  root lookup. `OrderTable::findState()` keeps the one-slot direct lookup
  inline, and partial execute/cancel calls the fused `reduceChecked()` path to
  validate page ownership and aggregate invariants while applying the level
  reduction.
- Fixed three-level bitmaps and cached best prices provide bounded best-level
  and top-ten discovery. Compact owner, page-summary, and book-summary arrays
  are separate from the 8,384-byte occupancy records, so resolving the next
  page does not stride through cold bitmap payloads. A predecessor/successor
  lookup reads at most five bitmap words per hierarchy. A singleton page keeps
  its sole level in the summary and does not touch its level bitmap; the bitmap
  is materialized only on the `1 -> 2` active-level transition. A full top-ten
  side performs at most nine successor traversals (18 bounded bitmap searches)
  and does not search for an unused eleventh result. Test-only counting
  observers execute the same templated search loops and verify the five-word
  bitmap and four-slot fallback bounds without production counters.
- A nonzero root handle is accepted only when it is in the committed arena and
  its owner record matches both `stock_locate` and `page_index`; corrupt root
  aliases fail before an aggregate can be mutated or page metadata indexed.

All capacities are fixed when `BookManager` is constructed. Price-page handles
are monotonic and are not recycled within a process session. Capacity failure
is returned as a typed mutation error; it does not trigger a hot-path heap
allocation.

`page_index` and `level_index` are address components, never price-window
bounds. For raw price `100000`, `page_index=1` and `level_index=34464`; joining
them produces the original value: `(1 << 16) | 34464 == 100000`. Prices `0`
and `UINT32_MAX` therefore use the same layout without recentering or a
configured market-price window.

### Session and market lifecycle

- Stock-directory `R` messages register identities before `SS` (an ITCH `S`
  message carrying event code `S`). At `SS`, all currently registered books
  are prepared against the already-fixed global stores.
- A repeated `R` after `SS` refreshes metadata without clearing the book. A new
  locate after `SS` activates one of the 65,536 descriptor slots allocated at
  startup, so the administrative path performs no new heap allocation. Reusing
  a locate for a different ticker is an identity error.
- System event `M` (End of Market Hours) does not stop book mutations. Neither
  does system event `E` (End of System Hours): `A/F/E/C/X/D/U` remain active
  for cleanup through that phase. In particular, message type `E` is Order
  Executed, while message type `C` is Order Executed With Price; neither is the
  like-named event code carried by an `S` message. Only system event `C` (End
  of Messages) is terminal; later ITCH book and directory messages are
  rejected.
- One process owns one Mold session. A session mismatch invalidates the channel
  and requires a new process; the book does not use partial generation tags or
  an in-place session reset.

### Deployment capacity profiles

`md_engine` has no universal order-book capacity default. It refuses startup
unless `ASTRA_BOOK_CAPACITY_PROFILE` names either the pinned acceptance fixture
or a fully explicit deployment profile. A custom profile must set every
sizing, evidence, and headroom field below; `ASTRA_BOOK_PREFAULT` is the one
optional field and defaults to `on`. There is no runtime growth or fallback to
the one-day fixture.

| Environment variable | Constraint and effect |
| --- | --- |
| `ASTRA_BOOK_CAPACITY_PROFILE` | Required audit-safe profile name. |
| `ASTRA_BOOK_CAPACITY_EVIDENCE_FILE` | Required canonical `astra_book_capacity_evidence_v1` manifest for a custom profile. |
| `ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256` | Required independently approved lowercase SHA-256 of the exact manifest bytes; the engine computes and compares it before using any value. |
| `ASTRA_ORDER_DIRECT_SLOTS` | Required nonzero direct-index domain; 16 bytes per slot. |
| `ASTRA_ORDER_FALLBACK_BUCKETS` | Required nonzero power of two; each 64-byte bucket has two ways and lookups remain bounded to two buckets. |
| `ASTRA_PRICE_PAGE_CAPACITY` | Required count of 2 MiB monotonic price pages in `1..UINT32_MAX-1`. |
| `ASTRA_PROFILED_MAX_ORDER_REF` | Required maximum order reference in the evidence; it must fit the direct table. |
| `ASTRA_PROFILED_UNIQUE_PRICE_PAGES` | Required lifetime count of distinct `(stock_locate, page_index)` pairs, not concurrent active pages. |
| `ASTRA_MIN_DIRECT_ORDER_HEADROOM` | Required positive absolute direct-slot reserve above the profiled maximum reference. |
| `ASTRA_MIN_PRICE_PAGE_HEADROOM` | Required positive absolute page reserve above the profiled lifetime demand. |
| `ASTRA_BOOK_PREFAULT` | Defaults to `on`; `off` is an explicit development-only choice. |

The custom manifest is canonical ASCII, has one final LF, no extra lines, and
uses this exact order:

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

The engine hashes the bytes it parses, checks the independent expected digest
and profile name, validates all bounds/headroom, and requires every duplicated
capacity environment value to equal the manifest. `md_engine
--book-storage-plan-only` reports that computed identity, the embedded corpus
and profiler identities, minimum/effective headroom, and the exact mapped-byte
plan without mapping the arenas.

For exact replay of the pinned 2019-01-30 acceptance fixture, set only:

```bash
ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-itch-20190130-acceptance-v1 \
  md_engine --book-storage-plan-only
```

That name selects `2^29` direct slots, `2^20` fallback buckets, 80,000 pages,
the pinned trace/profiler identities, and prefaulting. Capacity/evidence-file
overrides are rejected so the fixture remains reproducible; it is not a
production sizing recommendation.

On a Linux build with 4 KiB system pages and the current 64-byte descriptor-slot
ABI, the exact pinned-fixture book-storage plan is:

| Structure | Mapped/planned bytes |
| --- | ---: |
| Full `(stock_locate, page_index)` root | 17,179,869,184 |
| Prepared-locate flags | 2,097,152 |
| 80,000 aggregate pages | 167,772,160,000 |
| Page-owner records (80,001 slots) | 2,097,152 |
| Compact per-page summaries (80,001 slots) | 2,097,152 |
| Per-page, per-side occupancy bitmaps (160,002 slots) | 1,342,177,280 |
| Compact per-locate summaries (65,536 slots) | 2,097,152 |
| Per-locate, per-side occupancy bitmaps (131,072 slots) | 1,098,907,648 |
| `2^29` direct order states | 8,589,934,592 |
| `2^20` fallback buckets | 67,108,864 |
| **Mapped-array subtotal** | **196,058,546,176** |
| 65,536 preallocated book descriptor slots | 4,194,304 |
| **Planned book storage** | **196,062,740,480 bytes (182.598 GiB)** |

Anonymous mappings are zero-filled. A named Linux hot arena requires build
headers and runtime policy that accept both `MADV_HUGEPAGE` and anonymous-VMA
naming; construction fails before feed processing when either facility is
missing or denied. That startup success is still only a hint/identity check,
not proof of physical huge-page backing. Under the replay's explicit
`hot_arena_schema=redesign_v1`, every one of the eleven order, descriptor, and
price-store mappings is aligned and rounded to a whole 2 MiB extent. The EC2
acceptance harness does not treat that hint or kernel policy as proof: for each
schema-declared arena it requires the exact base, extent, Linux anonymous VMA
name, full-extent `AnonHugePages` residency in `/proc/PID/smaps`, and complete
placement on the requested NUMA node in `/proc/PID/numa_maps`. A missing or
unknown schema fails closed.
With prefaulting disabled, the same virtual capacities are reserved but pages
become resident on first touch; that mode is useful for development, not for an
accepted deterministic-latency run. The planner reports all eleven final
mapping extents and separately reports the logical descriptor payload;
allocator/kernel bookkeeping and replay buffers are outside that number.
`--storage-plan-only` is authoritative for the actual binary because system-
page size and the C++ descriptor ABI can change it.

A nominal 250 GB host exposes about 232.8 GiB in total, but total host memory is
not sufficient evidence for a one-node run. The acceptance harness binds all
allocations to one NUMA node and requires that selected node, its free memory,
and the active cgroup each cover the binary-derived plan plus the default
16 GiB reserve (213,242,609,664 bytes with the plan above). A host whose memory
is split into smaller NUMA nodes will fail this preflight even when its summed
memory is large enough; use a topology with one qualifying node or revise and
reapprove the binding policy.

The process prints the effective values as `book_storage` at startup. Invalid
capacities fail construction before feed processing.

## Build and correctness tests

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=ON
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

The book tests include full-domain prices (`0` and `UINT32_MAX`), 64-bit level
aggregation, cross-page best-price transitions, bounded fallback lookup,
transactional replacement failures, fixed-seed oracle replay, late `R` after
`SS`, independent handling of system-event `E` and order-message `E`, and
terminal system-event `C` behavior.

The retained raw trace profile is
[`docs/trace-profile-01302019.txt`](docs/trace-profile-01302019.txt). It records
the exact trace/profiler SHA-256 values and the complete profiler stdout used
for the capacity and far-price decisions below.

Local correctness evidence recorded on 2026-07-22 covers all 197 Release CTest
entries: the 192 non-socket entries passed together, and all five
`UdpReceiverTest` cases passed in a direct loopback-enabled run. ASan and UBSan
each passed all 185 non-receiver GoogleTest entries plus the five receiver cases
in the same direct rerun.

The final Release replay binary (SHA-256
`9ab0068a345e51485110ee91a1ce56101dd91291de8dde32f4d8ed9f5175af80`)
also passed the complete `01302019.NASDAQ_ITCH50` correctness replay against
the pinned trace SHA-256. It matched the exact 368,366,634-record and
11,245,883,092-byte gates, applied 363,118,215 book mutations, committed 68,941
price pages with zero capacity failures, ended with zero live orders in End of
Messages, and matched physical digest `11602566873588607264` and semantic
digest `13876319090171585636`. This was a non-prefaulted portable-clock
correctness run, not latency evidence; the controlled prefaulted x86 EC2/NUMA
digest and latency processes remain required for acceptance.
The two-process local record is retained in
[`docs/full-trace-replay-verification-20260722.txt`](docs/full-trace-replay-verification-20260722.txt).

The version-2 Release disassembly gate selected 94 functions from both parser
entrypoints through `dispatchMessage()` and the transitive in-binary mutation
closure. It rejected allocator, mapping, syscall, lock, lock-prefix, and
indirect-call targets; only the named cold `fail()` and `applyBookFailure()`
formatting boundaries may allocate. The same verifier passed 78 selected
functions in the pinned branch-5 adapter. Standard-library/ABI/PLT bodies and
the two named cold boundaries remain explicit trust boundaries, so this is a
strong static gate rather than a whole-program control-flow proof.

## Order-book benchmarks

The synthetic benchmark times ten bounded mutation/lookup shapes without UDP
or file I/O:

```bash
build/release/benchmarks/astra_order_book_benchmark \
  --iterations=100000 \
  --gate=direct_partial_execute:150:300:600 \
  --gate=remove_cross_page_best:150:300:600
```

It reports p50, p90, p99, p99.9, and maximum latency for these workload names:
`direct_partial_execute`, `direct_partial_execute_wide_pages`,
`direct_partial_cancel_wide_pages`, `add_existing_level`,
`delete_populated_level`, `replace_cross_page`, `remove_current_best`,
`remove_cross_page_best`, `fallback_partial_execute`, and
`fallback_four_slot_miss`. The two wide-page partial workloads keep one order
in each of up to 4,096 pages and visit references in a fixed-seed permutation.
They isolate the compact `OrderState -> PageOwner -> PriceLevel` path for ITCH
`E` and `X` without requiring page/book bitmap work.
`remove_cross_page_best` keeps one bid level in each of up to 4,096 pages and
allocates those pages in a fixed-seed permutation, so arena-handle order cannot
accidentally match price order. Every timed deletion empties a singleton page,
skips its level bitmap, and exercises the book-level page hierarchy. The
final workload fills both two-way candidate buckets during bounded, untimed
setup, then measures a known-missing reference whose lookup examines all four
fallback slots. `fallback_setup` reports the
successful fallback placement/lookup mix as `primary_lookups`,
`secondary_lookups`, and the count in each primary/secondary way. Repeat
`--gate=WORKLOAD:P50:P99:P99.9` to enforce different ceilings for any of the
ten named distributions. Each named workload may appear once, and all three
ceilings are required to be positive and monotonic. Unknown names, duplicate
workloads, incomplete gates, and inverted thresholds fail before measurement.
The example values are illustrative; use the approved ceilings from the
controlled host. On a non-x86 host, requesting any enabled latency gate fails
closed because the portable clock is not acceptance-equivalent to RDTSCP.
The legacy `--max-direct-p50-ns`, `--max-direct-p99-ns`, and
`--max-direct-p99-9-ns` options remain available for
`direct_partial_execute`; zero disables an individual legacy threshold, and
legacy options cannot be mixed with a named gate for that workload.

Profile the trace's capacity requirements without constructing the production
book:

```bash
build/release/benchmarks/astra_itch_trace_profile \
  data/itch/unzipped/01302019.NASDAQ_ITCH50
```

Replay the complete trace through the parser and redesigned book with sampled
message-to-book timing:

```bash
numactl --physcpubind=2 --membind=0 \
  build/release/benchmarks/astra_itch_book_replay_benchmark \
  data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  --prefault \
  --sample-every=64 \
  --warmup-book-messages=1000000 \
  --min-samples=1000 \
  --max-p50-ns=150
```

Replace CPU `2` and NUMA node `0` with an isolated CPU and its local memory
node on the test host. File reads are outside the timed interval. For each
sampled book message, replay calls `ItchParser::handleMessage()`, including its
empty/type/exact-length validation. Live Mold processing first validates the
entire packet's framing, type, and exact lengths, then calls
`handlePrevalidatedMessage()` and avoids repeating the length-table read. Both
entrypoints converge at the same audited `dispatchMessage()` and book-mutation
closure. Replay is therefore slightly conservative relative to live
per-message dispatch, while its timer deliberately excludes Mold framing,
sequencing, UDP, and file I/O.

Sampling uses one deterministic fixed-seed pseudo-random offset in each
complete block of `--sample-every` book messages. This preserves the approximate
sampling rate without repeatedly selecting the same position in a periodic
message-type pattern. Ready and final records identify that exact algorithm and
seed as
`sample_schedule_id=fixed_block_offset_v1_splitmix64_seed_61737472612d6974`;
the ready record also publishes the configured sample interval and warmup so a
monitor can reject the wrong schedule before releasing the start gate.
`--min-samples` (default `1000`) rejects an under-sampled run, and the main
result reports `sample_count`.

The replay capacities are runtime options:
`--direct-order-slots=N` (default `2^29`), `--fallback-buckets=N` (default
`2^20`), `--price-page-capacity=N` (default `80000`), and
`--sample-capacity=N` (default `8,388,608` timed samples). Unlike `md_engine`,
those three defaults intentionally reproduce the pinned 2019 acceptance
fixture; they are not live-deployment defaults. The replay does not prefault
the book unless `--prefault` is present. It always
sizes, touches, and clears the timed-sample vector before replay so sample
collection uses resident capacity. That vector and other benchmark/runtime
allocations are outside `planned_storage_bytes`.

Capacity overrides used for live acceptance must also supply
`--capacity-profile-name`, `--capacity-evidence-file`, and
`--capacity-evidence-sha256`. The replay loads the same canonical manifest in
process and publishes its computed identity and bound values in the storage
plan, ready marker, and final record. Small overrides without a manifest remain
available for development tests but report `capacity_profile_bound=0` and are
rejected by the acceptance harness.

For an accepted full-trace run, also pass `--expect-records=<exact-records>` and
`--expect-bytes=<exact-bytes>` using the values from `astra_itch_trace_profile`.
Those gates prevent a different or truncated input from being mistaken for the
approved trace. The replay also exits unsuccessfully on parser errors,
price-page exhaustion, nonzero final live orders, a final phase other than End
of Messages, or an enabled p50/p99/p99.9 gate failure. Set `--max-p99-ns` and
`--max-p99-9-ns` from the approved same-host baseline policy.

The main output is self-describing: it includes records/bytes, aggregate sample
count and distribution, sampling policy and warmup, prefault mode, configured
direct-order/fallback-bucket/price-page capacities, committed pages and capacity
failures, derived/effective storage bytes, resident sample capacity, post-warmup
minor/major faults, and the calibrated RDTSC overhead/frequency (or portable
clock fallback values). `--require-zero-post-warmup-faults` turns both fault
fields into a hard gate. `--storage-plan-only` prints the page-rounded book
mapping and descriptor plan without constructing the stores or allocating the
replay buffers; it is not a whole-process RSS estimate. Separate
`itch_book_replay_type` lines report
sample count and p50/p90/p99/p99.9/maximum for each of `A/F/E/C/X/D/U`; a type
with no selected sample is explicitly reported as unavailable.

For a prefaulted run, the benchmark flushes an `itch_book_replay_ready` record
only after all configured book mappings and timed-sample storage have completed
their startup touch pass and the trace has been consumed through System Event
`S`. No book mutation may appear before that boundary. The storage-plan, ready,
and final records repeat the same `hot_arena_schema`; ready and final also
publish the exact `prelude_records` and `prelude_bytes`. The ready record gives
every schema-declared arena's exact base and mapped extent. In harness-controlled
latency and digest runs, the benchmark then waits at a start gate while the
monitor captures and validates resident-memory, huge-page, VMA-name, and NUMA
evidence. The monitor releases the gate only after that evidence passes, so
startup work and `/proc` inspection cannot overlap the timed replay.

Run mutation-digest correctness separately, because observing post-mutation
state intentionally changes cache state and must not be mixed into an accepted
latency run:

```bash
build/release/benchmarks/astra_itch_book_replay_benchmark \
  data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  --sample-every=1024 \
  --min-samples=1000 \
  --expect-records=<exact-records> \
  --expect-bytes=<exact-bytes> \
  --mutation-digest
# Repeat on the same trace with
# --expect-mutation-digest=<physical value printed by the first run>
# --expect-semantic-mutation-digest=<semantic value printed by the first run>.
```

The benchmark rejects any combination of mutation-digest observation and a
nonzero latency threshold: correctness proof and latency acceptance require
separate processes. `state_checksum` remains a compact diagnostic, but the
accepted same-binary replay proof is the exact record/byte gates plus a
matching `mutation_digest`. That digest deliberately includes lookup path and
price-page handle, making it sensitive to the physical layout. The same
correctness run also emits the versioned
`semantic_mutation_digest_schema=applied_itch_book_semantics_v1_fnv1a64le`
digest. It covers each applied raw ITCH book message and logical post-state
(book live count/top, affected orders, and affected price levels), while
excluding handles, lookup paths, and addresses. Use its expected-value gate to
prove semantic equivalence across layout changes; neither digest observer runs
in latency mode.

For EC2 acceptance, use the same instance type, isolated core, NUMA node,
compiler, build flags, huge-page policy, clock policy, trace, warmup, and sample
interval for the branch-5 baseline and this branch. Run at least five fresh
processes and retain every result. Every candidate aggregate p50 must be at
most 150 ns. For the aggregate and each of `A/F/E/C/X/D/U`, every candidate
p99 and p99.9 must be no greater than that workload's worst branch-5 value
across all retained runs; p90 and maximum are informational. Also run
`perf stat` separately to record cycles, instructions, branches, branch misses,
cache misses, dTLB load misses, page faults, context switches, and CPU
migrations. Generic CI should gate correctness and bounded work, not wall-clock
nanoseconds.

The user-reported 310 ns branch-5 p50 motivated the target, but it is not a
repository-reproducible baseline. Branch 5 preserves a different DPDK
packet-path result, and the committed historical 310 ns row below is UDP
receive-path p99. Neither is comparable to the sampled book-only replay. Port
the identical benchmark to a pinned branch-5 snapshot, or supply approved
same-workload external output, before using 310 ns as comparison evidence. The
150 ns value is a target and gate, not a result; current-branch full-trace EC2
acceptance evidence is still pending.

### Controlled EC2 preparation

On the Ubuntu/Debian x86_64 test host, the setup script installs the build
toolchain plus the commands required by live acceptance (`numactl`, a
kernel-matched `perf`, `python3`, `taskset`, and the provenance utilities). Its
defaults match the paths used throughout this runbook:

```bash
scripts/setup_ec2.sh --run-tests
```

Place the trace at
`data/itch/unzipped/01302019.NASDAQ_ITCH50`, then verify it before either
variant is run:

```bash
test "$(sha256sum data/itch/unzipped/01302019.NASDAQ_ITCH50 |
  awk '{print $1}')" = \
  1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3
```

The setup script deliberately does not change host-wide policy. Select the CPU
and its local node, then make the following state true before acceptance:

- the machine is x86_64 and the selected CPU is online, belongs to the selected
  NUMA node, and appears in `/sys/devices/system/cpu/isolated`;
- the selected CPU's scaling governor is `performance` when a governor is
  exposed;
- swap is disabled, and transparent huge pages select `always` or `madvise`;
- `perf stat` can count the harness's complete event set on the selected CPU;
- the selected node and every finite cgroup limit have at least
  213,242,609,664 bytes available for the redesign plan plus its 16 GiB reserve.

These checks are useful before the authoritative harness preflight. The
following example uses CPU 2 and node 0; replace both assignments with the
selected pair:

```bash
CPU=2
NODE=0
uname -m
numactl --hardware
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/cpu"${CPU}"/cpufreq/scaling_governor 2>/dev/null ||
  true
cat /sys/kernel/mm/transparent_hugepage/enabled
awk '/^SwapTotal:/ {print}' /proc/meminfo
cat /proc/sys/kernel/perf_event_paranoid
numactl --physcpubind="${CPU}" --membind="${NODE}" true
perf stat -e cycles,instructions,branches,branch-misses,cache-misses,dTLB-loads,dTLB-load-misses,page-faults,context-switches,cpu-migrations \
  -- numactl --physcpubind="${CPU}" --membind="${NODE}" sleep 1
```

Domain isolation normally needs a host-specific bootloader change such as
`isolcpus=domain,<cpu> nohz_full=<cpu> rcu_nocbs=<cpu>` followed by a reboot.
Governor, IRQ affinity, swap, THP, cgroup, and perf-permission changes are also
host policy and remain manual. The file in `/sys/devices/system/cpu/isolated`,
not the boot command line by itself, is the acceptance fact. Likewise, THP
policy is only a prerequisite: the harness proves the redesign's actual
full-extent `AnonHugePages` residency after its ready gate.

### Build the pinned branch-5 baseline

Create the compatibility port as one named-branch commit in a separate
worktree. This avoids a detached `HEAD` and keeps the redesign worktree
available for the candidate run:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BASELINE_ROOT="$(dirname "${REPO_ROOT}")/astra-feed-engine-branch5-native-v1"
BASELINE_BRANCH=5-to-add-numa-acceptance
PINNED_BRANCH5=324d81a15ee52cc72f68873a1ced122923406df2
PORT_PATCH="${REPO_ROOT}/compat/branch5_native_v1/branch5-native-port.patch"
BRANCH5_CAPACITY_EVIDENCE="${REPO_ROOT}/compat/branch5_native_v1/capacity-evidence-01302019.txt"

test "$(sha256sum "${PORT_PATCH}" | awk '{print $1}')" = \
  914fde914dca97def9964059581ae8ce98dfce7a24b8d7659a183231a4093f7d
test "$(sha256sum "${BRANCH5_CAPACITY_EVIDENCE}" | awk '{print $1}')" = \
  05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282
git cat-file -e "${PINNED_BRANCH5}^{commit}"
git worktree add -b "${BASELINE_BRANCH}" "${BASELINE_ROOT}" \
  "${PINNED_BRANCH5}"
git -C "${BASELINE_ROOT}" apply --check "${PORT_PATCH}"
git -C "${BASELINE_ROOT}" apply --index "${PORT_PATCH}"
git -C "${BASELINE_ROOT}" commit \
  -m 'Add branch5 native acceptance compatibility port'
test "$(git -C "${BASELINE_ROOT}" rev-parse HEAD^)" = "${PINNED_BRANCH5}"
test "$(git -C "${BASELINE_ROOT}" rev-parse 'HEAD^{tree}')" = \
  492938730e6db91e84bdb1f8e25152536e81dbc0
bash "${BASELINE_ROOT}/scripts/build_branch5_native_replay.sh"
```

The four capacity arguments below are the pinned canonical branch-5 policy,
not tuning suggestions. Partial or different values are invalid. With the
pinned trace they prepare 8,713 books and describe 34,858 native ranges totaling
62,404,952,064 bytes. The 64 GiB admission value conservatively covers those
ranges; the default 16 GiB reserve is additional. The compatibility package's
canonical
[`capacity-evidence-01302019.txt`](compat/branch5_native_v1/capacity-evidence-01302019.txt)
at SHA-256
`05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282`
is the authority for these values; its derivation is retained in
[`capacity-evidence.md`](compat/branch5_native_v1/capacity-evidence.md).

### Run and compare the two variants

Run the baseline with deliberately non-gating `UINT64_MAX` latency ceilings.
This makes the retained branch-5 distributions the baseline rather than
silently applying the redesign target to the historical implementation:

```bash
"${REPO_ROOT}/scripts/run_order_book_acceptance.sh" \
  --binary "${BASELINE_ROOT}/build/branch5-native-release/benchmarks/astra_itch_book_replay_benchmark" \
  --expect-hot-arena-schema branch5_native_v1 \
  --trace "${REPO_ROOT}/data/itch/unzipped/01302019.NASDAQ_ITCH50" \
  --cpu "${CPU}" \
  --numa-node "${NODE}" \
  --expect-records 368366634 \
  --expect-bytes 11245883092 \
  --default-order-capacity 65536 \
  --price-node-capacity 163840 \
  --price-leaf-capacity 1048576 \
  --price-level-capacity 2097152 \
  --sample-capacity 8388608 \
  --planned-bytes 68719476736 \
  --reserve-bytes 17179869184 \
  --max-p50-ns 18446744073709551615 \
  --max-p99-ns 18446744073709551615 \
  --max-p99-9-ns 18446744073709551615 \
  --correctness-digest
```

Before the candidate run, commit the redesign on its named branch and require a
clean worktree. Rebuild from that commit, then run the same schedule. The p50
ceiling is real; the tail ceilings are intentionally non-gating here because
the cross-artifact comparator below gates every candidate p99 and p99.9 against
the corresponding worst retained branch-5 value:

```bash
test "$(git branch --show-current)" = 6-redesign-order-book-data-structure
test -z "$(git status --porcelain=v1 --untracked-files=all)"
cmake --build build/release --target astra_itch_book_replay_benchmark --clean-first

scripts/run_order_book_acceptance.sh \
  --binary build/release/benchmarks/astra_itch_book_replay_benchmark \
  --expect-hot-arena-schema redesign_v1 \
  --trace data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  --cpu "${CPU}" \
  --numa-node "${NODE}" \
  --expect-records 368366634 \
  --expect-bytes 11245883092 \
  --sample-capacity 8388608 \
  --reserve-bytes 17179869184 \
  --max-p50-ns 150 \
  --max-p99-ns 18446744073709551615 \
  --max-p99-9-ns 18446744073709551615 \
  --correctness-digest
```

Both commands print an `artifacts=...` path. Retain those two paths; a harness
`PASS` for the baseline means only that its run and evidence are valid, not that
branch 5 met a performance target.

The script evaluates only the supplied binary and ceilings; it does not run or
compare another branch. It runs at least five latency processes and requires
every one to pass, retains every stdout/stderr and a worst-of-all-runs summary,
then performs both implementation and semantic digest discovery and
verification in separate processes when requested. The storage-plan, ready,
and final records must identify one supported hot-arena schema, and the ready
and final records must identify the exact fixed sampling schedule, interval,
and warmup. Before a live run the harness asks the binary for
`--storage-plan-only`, verifies every run against that exact capacity plan,
checks that its total equals the schema-declared mapped extents, and requires
the selected node and every finite visible cgroup ancestor to cover it plus a
16 GiB reserve. It also requires x86_64,
`numactl`, a named non-detached clean Git branch, and a CMake `Release` build
with identifiable compiler metadata. The existing build directory is discovery
evidence, not proof that its executable came from that source. The harness
exports the verified commit with `git archive`, configures a fresh temporary
out-of-tree graph under an empty recorded build environment, and performs a
clean-first build of only `astra_itch_book_replay_benchmark`. The supplied,
freshly built, and both archived executables must be byte-for-byte identical
before any run starts. The same target-level proof applies to the reviewed
branch-5 compatibility commit. It verifies CPU/node placement and that
the candidate CPU belongs to Linux's domain-isolated CPU list; when a scaling
governor is exposed, it must be `performance`. The harness rejects enabled swap
by default, enforces zero post-warmup faults and resident sample storage, and
records boot/CPU identity, topology, inherited binding, overcommit, commit,
cgroup, and THP state. After the ready marker, each
retained full `smaps`, `smaps_rollup`, and `numa_maps` snapshot must report zero
swapped pages, anonymous resident bytes and NUMA pages covering the plan, and at
least 99% of anonymous pages on the chosen node. For `redesign_v1`, each of the
eleven exact named arena VMAs must additionally be 2 MiB aligned, wholly
reported as `AnonHugePages`, and wholly resident on the requested node. The
monitor validates those files before releasing the benchmark's start gate;
failed or unavailable evidence terminates the still-blocked process.

The backend-specific replay drivers implement the same trace, sampling,
digest, post-System-S ready/start-gate, and output contracts. The harness
accepts the pinned baseline adapter only when it declares
`hot_arena_schema=branch5_native_v1`. That backend does not claim
the redesign's eleven VMAs or THP guarantees. After replaying the `R`/System-`S`
startup prelude, it seals the prepared-book universe and atomically writes a
`branch5_native_ranges_v1` manifest for its six shared price vectors and four
allocator-backed vector spans per prepared locate. The harness recomputes the
manifest digest, checks deterministic kind/locate ordering and storage-plan
sizes, proves every positive span lies in anonymous process VMAs, and applies
the process-wide 99% NUMA gate without double-counting shared heap pages.
Because branch-5 per-book capacity is unknown before that prelude, a live
baseline run must supply a conservative `--planned-bytes` admission bound; the
ready snapshot must still cover the complete manifest byte total. Unknown,
empty, or cross-record schema identities fail closed.

`book_universe_sealed=1` is evidence about the branch-5 compatibility adapter,
not the redesign lifecycle. The redesign never seals directory membership:
repeated and genuinely new `R` messages remain supported after `SS` and through
system-event `E`, until terminal system-event `C`.

After producing both artifact directories on the same controlled host, run the
fail-closed comparator:

```bash
scripts/compare_order_book_acceptance.sh \
  --baseline-dir <branch5-artifact-dir> \
  --candidate-dir <redesign-artifact-dir> \
  --expect-baseline-branch 5-to-add-numa-acceptance \
  --expect-baseline-commit <branch5-port-commit> \
  --expect-candidate-branch 6-redesign-order-book-data-structure \
  --expect-candidate-commit <redesign-commit> \
  --expect-candidate-capacity-evidence-sha256 1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3 \
  --expect-trace-sha256 1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3 \
  --expect-records 368366634 \
  --expect-bytes 11245883092
```

For the built-in pinned candidate, the approved capacity-evidence SHA is the
pinned trace SHA shown on the next line; the comparator also exact-checks the
built-in profiler identity, three capacities, profiled demands, and
minimum/effective headrooms. For a custom candidate it is the independently
approved SHA-256 of the canonical capacity manifest.

A separate default-required `perf stat` process records the approved
hardware-counter set without contaminating accepted latency distributions.
Every live run unconditionally hashes the trace before and after replay; the
legacy `--hash-trace` option is a compatibility no-op. Artifacts archive the
tested binary, acceptance harness, hot-path verifier and its disassembly report,
the custom capacity evidence manifest when present,
the exact exported source-tree archive, fresh-built binary, NUL-delimited build
argv, configure/build logs, fresh and supplied `CMakeCache.txt` files, available
build graphs/compile commands, compiler version, complete pre-build/post-build/
post-run Git state, and pre/post hashes and file state for every live input.
Provenance version 2 and the comparator both require the clean-source build
attestation; a stale or manually substituted executable cannot become
acceptable merely by updating its standalone hashes. Acceptance also fails if
the trace, binary, harness, verifier, build cache, branch, commit, or worktree
changes. The final `manifest.sha256` covers every
regular artifact and can be checked with `sha256sum -c manifest.sha256`. A
comparison parses only private temporary copies whose manifests must match the
stable source manifests before and after copying; ensure `TMPDIR` has room for
both artifact directories.

A conservative `--planned-bytes` override may raise, but never lower, the
binary-derived plan; `--reserve-bytes` adjusts headroom. `--dry-run` validates
and prints the plan without Linux-only checks or writes, so it is safe to
exercise on macOS. `scripts/setup_ec2.sh` builds benchmark binaries by default;
`--no-benchmarks` is an explicit opt-out.

Timing and correctness builds are portable. On x86/x86_64, `rdtsc()` uses
RDTSCP; on other architectures it uses the monotonic clock fallback. Therefore
non-x86 hosts can run tests and benchmarks, but they are not valid for the EC2
cycle/latency acceptance gate.

## Linux A/B Replay

This runbook assumes:

- engine host: runs `md_engine`
- sender host: runs two `itch_moldudp_sender` processes through
  `scripts/run_itch_ab_senders.sh`
- ITCH file: `data/itch/unzipped/01302019.NASDAQ_ITCH50`
- packet shape: `20` ITCH messages per MoldUDP64 packet

### Receiver

Start the engine first. Keep drop metrics enabled for benchmark validation.

```bash
ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_LATENCY_METRICS=on \
ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-itch-20190130-acceptance-v1 \
ASTRA_BOOK_PREFAULT=on \
./build/release/md_engine 0.0.0.0 9000 0.0.0.0 9001
```

`ASTRA_LATENCY_METRICS=off` disables packet-level latency recording. Keep it on
for transport measurements and off for isolated book microbenchmarks. Full
prefaulting is synchronous and intentionally expensive; wait for the receiver
to finish startup before starting either sender.

Receiver controls are:

| Environment variable | Default | Meaning |
| --- | ---: | --- |
| `ASTRA_CPU` | unset | Pin the engine thread; startup fails if the requested pin cannot be established. |
| `ASTRA_UDP_RX` | `recv` | Set to `recvmmsg` or `batch` for the Linux batched receiver. |
| `ASTRA_UDP_BATCH_SIZE` | `8` | Batch size when `recvmmsg` is selected; the receiver clamps it to `1..64`. |
| `ASTRA_UDP_DROP_METRICS` | `off` | Enable `SO_RXQ_OVFL` reporting on the ordinary receive path. |
| `ASTRA_LATENCY_METRICS` | `on` | Enable packet-level latency sampling/reporting. |

### Sender

For a realistic pre-market shape without waiting the full 5.5 hours, use
timestamp replay mode. It follows the ITCH timestamps from `SS` through `SQ`,
scaled by `ASTRA_PREMARKET_SPEEDUP`.

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=30 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  10000
```

The final `10000` is the normal packet rate per line outside the timestamp-paced
pre-market window.

## Replay Modes

### Timestamp Mode

Recommended for market-shape simulation.

```text
ASTRA_PREMARKET_REPLAY_MODE=timestamp
ASTRA_PREMARKET_SPEEDUP=33
ASTRA_SS_PAUSE_SECONDS=30
```

For the sample ITCH file, `SS` is around `04:00:00` and `SQ` is around
`09:30:00`, so the real pre-market window is about `19,800` seconds.

| Speedup | Approximate pre-market duration |
| ------: | ------------------------------: |
|     `1` |                       5.5 hours |
|    `10` |                      33 minutes |
|    `33` |                      10 minutes |
|   `165` |                       2 minutes |

`ASTRA_SS_PAUSE_SECONDS` adds a quiet pause immediately after sending `SS`, so
the receiver can finish bulk descriptor preparation before pre-market order
flow resumes. The global arenas were already created and prefaulted at process
startup.

### Flat Stress Mode

Use flat mode when you want a deterministic stress window instead of the real
ITCH burst shape.

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_PREMARKET_SECONDS=600 \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  50000
```

`ASTRA_PREMARKET_SECONDS=600` spreads the whole `SS` to `SQ` segment evenly
over 10 minutes. This is smoother than timestamp mode and is useful for capacity
testing.

### Positional Arguments

The wrapper also supports positional controls:

```text
run_itch_ab_senders.sh \
  <itch_file> <dest_ip> <port_a> <port_b> \
  <msgs_per_packet> <session> <pkt_per_second> \
  [premarket_seconds] [ss_pause_seconds] [premarket_replay_mode] [premarket_speedup]
```

Timestamp-mode example using only positional arguments:

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  10000
```

## Validation

A clean run should end with:

```text
channel_status_name=Good
line_a_kernel_drops=0
line_b_kernel_drops=0
```

For a completed sender stream, `sender next_seq` should match receiver
`channel_next_seq`. If the sender is interrupted first, a small tail delta is
normal. Persistent `GapDetected`, nonzero kernel drops, or a large
sender/receiver sequence gap means the run should not be used as a clean latency
baseline.

## Historical transport results

The results below predate the redesigned global order table and paged price
store. They document the A/B transport test shape, but they are not acceptance
evidence for this branch. Re-run the commands above on the fixed EC2 setup
before publishing a redesign comparison.

### Full-Day Flat Replay

`recv`, A/B lines, `20` messages per packet, `ASTRA_PREMARKET_SECONDS=600`.

| Post-`SQ` rate per line | SS pause | Status | Kernel drops | Final sequence |      p50 |      p99 |     p99.9 |    p99.99 |
| ----------------------: | -------: | ------ | -----------: | -------------: | -------: | -------: | --------: | --------: |
|           `10000 pkt/s` |   `30 s` | `Good` |      `0 / 0` |    `102437301` | `170 ns` | `570 ns` | `1535 ns` | `1935 ns` |
|           `50000 pkt/s` |  `120 s` | `Good` |      `0 / 0` |    `368366635` | `116 ns` | `310 ns` | `1023 ns` | `1775 ns` |

### Timestamp-Shaped Replay

`recv`, A/B lines, `20` messages per packet,
`ASTRA_PREMARKET_REPLAY_MODE=timestamp`, `ASTRA_PREMARKET_SPEEDUP=33`.

| Post-`SQ` rate per line | SS pause | Status | Kernel drops | Final sequence |      p50 |      p99 |     p99.9 |    p99.99 |
| ----------------------: | -------: | ------ | -----------: | -------------: | -------: | -------: | --------: | --------: |
|           `10000 pkt/s` |   `30 s` | `Good` |      `0 / 0` |     `48386301` | `138 ns` | `320 ns` |  `899 ns` | `3759 ns` |
|           `50000 pkt/s` |  `120 s` | `Good` |      `0 / 0` |    `368366635` | `140 ns` | `612 ns` |  `870 ns` | `1423 ns` |
|          `100000 pkt/s` |  `120 s` | `Good` |      `0 / 0` |    `368366635` | `150 ns` | `541 ns` | `1727 ns` | `2127 ns` |

These retained tables contain percentile columns only, not a maximum-latency
column. They are historical packet-path context and must not be used as the
book-only acceptance distribution.

## Test Environment

```text
Sender: c7i.4xlarge with 100 GB gp3 EBS
Engine: r7iz.8xlarge
Transport: regular UDP recv, redundant A/B lines
```
