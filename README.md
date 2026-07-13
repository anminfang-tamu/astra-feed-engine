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
                                  +-> global OrderRefDirectory
                                  +-> global OrderArena
                                  +-> per-book radix roots backed by shared pools
```

Stock Directory messages establish the book universe. At Start of System Hours
(`SS`) that universe is sealed, so late book creation is rejected instead of
allocating on the hot path. Capacity exhaustion never falls back to another
container; it marks the affected state invalid and increments a failure counter.

The default directory, order, and price-pool configuration reserves roughly
2.6 GiB. Size these controls together for the target feed:

| Environment variable                 |                                        Default |
| ------------------------------------ | ---------------------------------------------: |
| `ASTRA_ORDER_DIRECTORY_SLOTS`        |                                     `16777216` |
| `ASTRA_ORDER_POOL_CAPACITY`          | directory limit (`8388608` with default slots) |
| `ASTRA_PRICE_INTERNAL_NODE_CAPACITY` |                                       `163840` |
| `ASTRA_PRICE_LEAF_CAPACITY`          |                                      `1048576` |
| `ASTRA_PRICE_LEVEL_CAPACITY`         |                                      `2097152` |

`ASTRA_ORDER_DIRECTORY_SLOTS` must be a power of two, and the directory holds
at most half as many live mappings as slots. Price-pool defaults scale down when
the configured order pool is smaller.

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
- Books: all directory, order-reference, mutation, allocation, rejection,
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

## Current validated baseline

The current post-redesign baseline was recorded on 2026-07-13 at clean Git
revision `3e11646f4931`. It replayed all `368366634` ITCH records through the
fixed global order directory, global order arena, and shared price-radix pools.

- Receiver: `r7iz.8xlarge`, Release, IPO off, CPU 2 on NUMA node 0, DPDK packet
  latency mode, burst 16.
- Sender: `c7i.4xlarge`, timestamp-shaped A/B replay, `100000` packets/second per
  line, 120-second `SS` pause, 1000 ns B-line delay.
- Integrity: `channel_status_name=Good`, final sequence `368366635`, both lines
  received `18418332` packets, and `imissed/ierrors/rx_nombuf=0/0/0`.

|      p50 |      p90 |      p99 |    p99.9 |    p99.99 |        Mean |
| -------: | -------: | -------: | -------: | --------: | ----------: |
| `224 ns` | `294 ns` | `428 ns` | `847 ns` | `1071 ns` | `230.87 ns` |

Peak pool usage was `1742866 / 8388608` orders, `71065 / 163840` internal price
nodes, `587520 / 1048576` price leaves, and `707130 / 2097152` occupied price
levels. All book and sequencing failure counters were zero.

Latency is measured from the receiver's user-space timestamp through MoldUDP64
decoding, ITCH parsing, and book updates. In DPDK packet mode the timestamp is
taken after `rte_eth_rx_burst()` returns and immediately before frame parsing;
kernel/NIC work before that point is outside the measurement.
