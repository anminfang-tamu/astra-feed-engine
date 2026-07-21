# astra-feed-engine

A low-latency C++20 feed handler for MoldUDP64-wrapped NASDAQ TotalView-ITCH
5.0. It consumes redundant A/B feeds, validates sequencing and session state,
and maintains fixed-capacity in-memory order books.

## Key points

- End-to-end path: `UDP or DPDK -> MoldUDP64 -> ITCH 5.0 -> order books`.
- Redundant A/B lines share one sequence stream with first-arrival arbitration,
  gap detection, and session validation.
- Kernel `recv`, Linux `recvmmsg`, and Linux DPDK receivers use the same decoder
  and book path.
- Order references, orders, radix nodes, leaves, and price levels use
  preallocated fixed-capacity storage.
- Drops, gaps, allocation failures, invalid book state, and pool exhaustion are
  reported explicitly and invalidate a benchmark run.

## Architecture

```text
A/B network feeds
       |
       v
IMarketDataSource (recv, recvmmsg, or DPDK)
       |
       v
MoldUdpDecoder -> ItchParser -> BookManager
                                  |
                                  +-> OrderBook[stock_locate]
                                      +-> local order-ref index
                                      +-> local fixed OrderArena
                                      +-> local price-radix root
                                              |
                                              +-> shared fixed price pools
```

Stock Directory messages establish the book universe. At Start of System Hours
(`SS`) every registered book allocates and page-touches its local reference map
and order arena, then the universe is sealed. Late book creation is rejected
instead of allocating during live processing. The per-book map and arena never
resize, and the shared price pools are also fixed. Capacity exhaustion never
falls back to another container; it marks the affected state invalid and
increments a failure counter. A book message before readiness is fatal and
cannot lazily allocate a book. The parser latches readiness failure, refuses
the Market Hours transition, and the decoder marks the channel invalid.

The local reference index stores `{uint64_t order_ref, uint32_t local_index}` in
16-byte open-addressed entries. Erase uses backward-shift compaction inside the
preallocated array. Those entry copies do not allocate; avoiding permanent
tombstones prevents full-day churn from turning misses into full-table probes.

Order capacity is selected per symbol. The built-in defaults are 64K orders for
ordinary symbols, 256K for Active, 1M for Hot, and 4M for UltraHot symbols. A
local map reserves four slots per configured order, keeping its maximum live
load at 25%; together with the local arena this is about 100.125 bytes per
configured order. This intentionally trades memory for shorter probe chains.
Non-power-of-two custom capacities round the map to the next power of two and
can reserve more. A roughly 8,700-symbol universe with the built-in tier list is
about 56 GiB of local book storage, before the shared price pools' approximately
2.3 GiB and the decoder's other fixed buffers.

| Environment variable                 |                                        Default |
| ------------------------------------ | ---------------------------------------------: |
| `ASTRA_BOOK_ORDER_CAPACITY`           | `65536` for symbols without a higher tier       |
| `ASTRA_PRICE_INTERNAL_NODE_CAPACITY` |                                       `163840` |
| `ASTRA_PRICE_LEAF_CAPACITY`          |                                      `1048576` |
| `ASTRA_PRICE_LEVEL_CAPACITY`         |                                      `2097152` |
| `ASTRA_STOP_ON_DECODE_ERROR`         |                                           `true` |

Active/Hot/UltraHot ticker membership is currently compiled in
`BookCapacity.hpp`. Library callers can set an explicit per-locate capacity
before Stock Directory processing and that value takes precedence over the
tier; the shipped executable does not yet load a per-symbol capacity file. Size
and bind the complete working set for the deployment NUMA node. The sender's
`ASTRA_SS_PAUSE_SECONDS` exists to leave time for the synchronous `SS`
allocation and first-touch phase before live-rate replay resumes.

## Build and test

Requirements:

- Linux on x86/x86_64; the current timing path uses Linux `rdtsc` calibration
- CMake 3.20 or newer
- a C++20 compiler
- zlib development files
- `libdpdk` only when building the optional DPDK receiver

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

GoogleTest is discovered locally or fetched by CMake when tests are enabled.

## Run a Linux A/B replay

Historical ITCH data is not committed to this repository. Place the uncompressed
file at, for example:

```text
data/itch/unzipped/01302019.NASDAQ_ITCH50
```

Start the receiver first and wait for `Engine started`:

```bash
ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
./scripts/run_engine_udp.sh
```

Then start the synchronized sender on the replay host. This example preserves
the ITCH pre-market timing shape at 33x speed and sends `100000` packets/second
per line after the opening transition:

```bash
RECEIVER_IP=172.31.32.18  # replace with the receiver feed-interface IP
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  "${RECEIVER_IP}" 9000 9001 20 "ASTRA     " 100000
```

The sender reads and packetizes the ITCH file once, then sends identical
MoldUDP64 packets over A and B. `ASTRA_LINE_B_DELAY_NS` adds deterministic test
skew; it is not a model of real exchange-path behavior.

For controlled benchmarks, pin the sender and receiver CPUs to the NUMA node
local to their feed NICs with `ASTRA_NUMA_NODE` and
`ASTRA_NUMA_MEM_POLICY=membind`. Set `ASTRA_LATENCY_METRICS=off` only for a
throughput-only run.

Replay shaping is intentionally small:

| Mode             | Controls                                                           | Use                                    |
| ---------------- | ------------------------------------------------------------------ | -------------------------------------- |
| Timestamp-shaped | `ASTRA_PREMARKET_REPLAY_MODE=timestamp`, `ASTRA_PREMARKET_SPEEDUP` | Real ITCH burst shape, time-compressed |
| Flat stress      | `ASTRA_PREMARKET_SECONDS=<seconds>`                                | Smooth, deterministic capacity load    |

## DPDK receiver

DPDK is an optional Linux transport replacement; it does not change MoldUDP64,
ITCH parsing, sequencing, or book handling.

```bash
FEED_PCI=0000:28:00.0  # replace with the DPDK-bound feed NIC
sudo env \
ASTRA_CPU=2 \
ASTRA_NUMA_NODE=0 \
ASTRA_DPDK_PORT_ID=0 \
ASTRA_DPDK_BURST_SIZE=16 \
ASTRA_DPDK_LATENCY_MODE=packet \
ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2 --allow ${FEED_PCI}" \
./scripts/run_engine_dpdk.sh
```

The DPDK burst size must be divisible by 8. Flow filtering is on by default;
use `ASTRA_DPDK_FLOW_FILTER=off` if the active PMD rejects isolated
IPv4/UDP `rte_flow` rules. See
[DPDK Setup on AWS EC2 Linux](docs/dpdk-aws-ec2-setup.md) for hugepages,
secondary-ENI binding, VFIO no-IOMMU mode, validation, and NIC restoration.

## Validation

Do not record performance results unless all of these gates pass:

- Sender: `completion=complete`, both lines start at sequence `1`, and A/B send
  failures are zero.
- Receiver: the first overall, A-line, and B-line sequences are `1`;
  `channel_status_name=Good`; session mismatches and buffered gaps are zero.
- Completion: receiver `channel_next_seq` exactly equals sender `next_seq`.
- Kernel UDP: `line_a_kernel_drops=0` and `line_b_kernel_drops=0`.
- DPDK: `imissed=0`, `ierrors=0`, and `rx_nombuf=0`.
- Books: all local-index, order-reference, mutation, allocation, rejection,
  invalid-book, and price-pool exhaustion counters are zero.

The engine exits nonzero when final channel or book-state validation fails.
An interrupted sender, any remaining sequence delta, or any transport drop
invalidates the run even if the latency percentiles look good.

For transport-independent full-file correctness testing:

```bash
cmake -S . -B build-perf \
  -DASTRA_BUILD_TESTS=OFF \
  -DASTRA_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf --target astra_itch_replay_benchmark --parallel
./build-perf/benchmarks/astra_itch_replay_benchmark \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50
```

## Historical validated baseline

The clean post-redesign run (`3e11646f4931`, 2026-07-13) replayed all
`368366634` records through the global order arena and shared price-radix pools.
It used DPDK packet mode with burst 16 and timestamp-shaped A/B traffic at
`100000` packets/second per line.

|      p50 |      p90 |      p99 |    p99.9 |    p99.99 |        Mean |
| -------: | -------: | -------: | -------: | --------: | ----------: |
| `224 ns` | `294 ns` | `428 ns` | `847 ns` | `1071 ns` | `230.87 ns` |

The final sequence was `368366635`; both lines received `18418332` packets, and
all transport, sequencing, book, and allocation failure counters were zero. Peak
global orders were `1742866 / 8388608`; price leaves were the tightest shared
pool at `587520 / 1048576`. Latency covers user-space frame parsing through book
updates. The `172.759 us` maximum coincided with manual Ctrl+C shutdown and is
excluded from steady-state comparison.

That measurement belongs to the former global-order design. It is retained as
a historical comparison point, not as performance evidence for the current
per-book hybrid. Record a new clean full replay and same-binary DPDK run before
claiming latency parity or improvement for this branch.
