# astra-feed-engine

AstraFeed is a low-latency C++ market data feed handler for MoldUDP64-wrapped
NASDAQ ITCH 5.0 replay. The primary benchmark setup uses redundant A/B UDP
sender lines feeding one `md_engine` receiver.

## Order-book architecture

The live book path has one authoritative copy of each kind of state:

- A fixed global `OrderRefDirectory` maps every nonzero 64-bit ITCH order
  reference to `(stock_locate, order_pool_index)`. It uses a preallocated
  half-loaded open-addressed table; there is no per-book ID-map fallback.
- A fixed global `OrderArena` stores 32-byte `Order` records. The record keeps
  its full order reference, raw ITCH price, quantity, FIFO links, side, and
  price-level handle. All books share the arena, so quiet symbols do not reserve
  large per-symbol order pools.
- Each book has a constant-depth four-byte radix root for the complete
  `uint32_t` ITCH price domain. Internal nodes, bid/ask leaves, and 64-bit
  aggregate price levels come from shared fixed pools. Best price and top-ten
  traversal use the same radix/bitmap state; there is no duplicate ordered
  index.
- Stock Directory messages prepare the registered book universe before live
  flow. At Start of System Hours the universe is sealed; an unexpected late
  book request is rejected and counted instead of allocating on the hot path.

All directory, order, and price-pool storage is allocated and page-touched at
startup. Capacity exhaustion never falls back or silently drops into another
structure: it latches the affected book invalid and increments an explicit
shutdown counter.

Production defaults and overrides:

| Setting | Default | Meaning |
| --- | ---: | --- |
| `ASTRA_ORDER_DIRECTORY_SLOTS` | `16777216` | Hash slots; maximum live mappings are half this value |
| `ASTRA_ORDER_POOL_CAPACITY` | directory maximum | Global concurrent live-order capacity |
| `ASTRA_PRICE_INTERNAL_NODE_CAPACITY` | `163840` | Shared radix internal nodes |
| `ASTRA_PRICE_LEAF_CAPACITY` | `1048576` | Shared 256-price radix leaves |
| `ASTRA_PRICE_LEVEL_CAPACITY` | `2097152` | Shared occupied side/price levels |

These are operational envelopes, not assumptions about a particular trading
day. Price-pool defaults scale down with a smaller configured order pool. Size
all pools for expected concurrent live state plus headroom, then accept a run
only when every directory, order-pool, price-pool, and local-invalid counter is
zero. The production defaults reserve roughly 2.6 GiB for these three core
structures, excluding the separate fixed sequencing/gap state; smaller
deployments should lower the explicit capacities together.

## Linux A/B Replay

This runbook assumes:

- engine host: runs `md_engine`
- sender host: runs one synchronized redundant `itch_moldudp_sender` process
  through `scripts/run_itch_ab_senders.sh`; it uses one replay source and clock
  plus separately pinned A/B line threads
- ITCH file: `data/itch/unzipped/01302019.NASDAQ_ITCH50`
- packet shape: `20` ITCH messages per MoldUDP64 packet

Wait for the `Engine started` line before launching the sender. A clean
dual-feed replay must observe sequence `1` as the first packet overall and on
both configured A/B paths.

### Receiver

Start the engine first. Keep drop metrics enabled for benchmark validation.

```bash
ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./scripts/run_engine_udp.sh
```

Per-stage timing is not currently wired into `md_engine`; packet-level latency
is controlled by `ASTRA_LATENCY_METRICS`. With
`ASTRA_LATENCY_METRICS=off`, receivers skip `rdtsc` capture entirely and the
decoder performs no timestamp conversion, providing a genuine throughput mode.

The UDP and DPDK engine wrappers reconfigure and incrementally rebuild
`md_engine` before every launch, default to `Release`, and print the Git SHA,
worktree state, and build type used for the run. Set `ASTRA_BUILD_TYPE` to
override the build type. Set `ASTRA_ENABLE_IPO=ON` to request compiler-supported
interprocedural optimization/LTO; configuration fails explicitly when the
selected toolchain cannot provide it.

### NUMA Binding

`scripts/setup_ec2.sh` reports NUMA topology by default, including
`numactl --hardware`, `lscpu`, and the NUMA node for the default network
interface when Linux exposes it under `/sys/class/net`. Use `--numa-iface IFACE`
when the benchmark traffic uses a non-default interface.

For the engine host, choose `ASTRA_CPU` from the same NUMA node as the receiving
network interface, then enable the repo wrapper's `numactl` path:

```bash
ASTRA_NUMA_NODE=0 \
ASTRA_NUMA_MEM_POLICY=membind \
ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./scripts/run_engine_udp.sh 0.0.0.0 9000 0.0.0.0 9001
```

`ASTRA_NUMA_MEM_POLICY=membind` is the strict benchmark default. Use
`localalloc` for a softer policy, `preferred` to prefer the selected node, or
`none` to apply only CPU binding. `scripts/run_itch_ab_senders.sh` honors the
same `ASTRA_NUMA_NODE` and `ASTRA_NUMA_MEM_POLICY` variables. Choose
`ASTRA_CPU_A` and `ASTRA_CPU_B` from that node. The wrapper gives the process a
CPU mask containing both CPUs, and each A/B line thread pins itself to its
assigned CPU.

Automatic NUMA balancing is not changed by default. If you want the EC2 setup
script to disable it for benchmark determinism during the current boot, run with
`--apply-numa-tuning`.

### Secondary ENI Feed Interface

On EC2, attach the secondary ENI to the receiver instance first, usually as
device index `1`. Linux often exposes that interface as `ens6` or `eth1`, but
verify with `ip -br link` instead of hardcoding the name.

Configure the OS side of the attached ENI with:

```bash
./scripts/setup_secondary_eni.sh
```

If auto-detection is not enough, pass the interface explicitly:

```bash
./scripts/setup_secondary_eni.sh --iface ens6
# or
./scripts/setup_secondary_eni.sh --iface eth1
```

Use the secondary ENI private IP as the sender destination. Keep the receiver
bind address on `0.0.0.0`; the kernel will deliver packets that arrive on the
secondary ENI to `md_engine`:

```bash
ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./scripts/run_engine_udp.sh 0.0.0.0 9000 0.0.0.0 9001
```

For locality checks, point the EC2 setup report at the feed interface:

```bash
./scripts/setup_ec2.sh --numa-iface ens6 \
  --no-apt --no-data-dir --no-submodules --no-configure --no-build
```

On `r7iz.8xlarge`, this is useful for separating feed traffic by interface and
private IP. It does not double network bandwidth because the instance exposes
one network card.

### DPDK Receiver

The DPDK path replaces only the UDP socket receiver. `MoldUdpDecoder`,
`ItchParser`, and `BookManager` still consume the same MoldUDP64 `PacketView`
payload used by the regular UDP path.

Build and run the DPDK receiver on Linux with:

```bash
ASTRA_CPU=2 \
ASTRA_NUMA_NODE=0 \
ASTRA_DPDK_PORT_ID=0 \
ASTRA_DPDK_BURST_SIZE=8 \
ASTRA_DPDK_LATENCY_MODE=packet \
ASTRA_DPDK_FLOW_FILTER=off \
ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2" \
./scripts/run_engine_dpdk.sh 0.0.0.0 9000 0.0.0.0 9001
```

DPDK is off by default. The wrapper incrementally builds `build-dpdk/md_engine`
with `-DASTRA_ENABLE_DPDK=ON` and runs it with `ASTRA_RX=dpdk`. Validate the host
first with hugepages, NIC binding, PMD availability, and `testpmd` RX.
`ASTRA_DPDK_BURST_SIZE` must be divisible by `8`.
`ASTRA_DPDK_LATENCY_MODE=packet` timestamps each accepted packet before DPDK
frame parsing; use `burst` only when you want the older burst-level queueing
view.
`ASTRA_DPDK_FLOW_FILTER=on` is the default. It enables DPDK isolated mode and
installs `rte_flow` rules for the configured IPv4/UDP destination ports before
the receiver starts polling. If a PMD rejects the flow rules, set
`ASTRA_DPDK_FLOW_FILTER=off` to return to userspace filtering.

For the full AWS EC2 setup flow, including secondary-ENI binding, VFIO
no-IOMMU mode, and restoring the NIC to Linux, see
`docs/dpdk-aws-ec2-setup.md`.

Clean DPDK acceptance requires all common [Validation](#validation) gates plus:

```text
imissed=0
ierrors=0
rx_nombuf=0
```

DPDK `rx_stats` should also show most packets on `fast_path`; fallback packets
mean the frame shape did not match the fixed Ethernet/IPv4/UDP hot path and was
handled by the general parser.

### Sender

For a realistic pre-market shape without waiting the full 5.5 hours, use
timestamp replay mode. It follows the ITCH timestamps from `SS` through `SQ`,
scaled by `ASTRA_PREMARKET_SPEEDUP`.

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
ASTRA_NUMA_NODE=0 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.18 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  100000
```

The feeder reads and packetizes the ITCH file once, then its two pinned line
threads send identical bytes, session, and sequence numbers to A and B under
one replay clock. It dispatches A first and B after the configured line delay;
both line sends complete before the replay advances. The final `100000` is the
normal packet rate on each line outside the timestamp-paced pre-market window.

At natural EOF, the final `sender_stats` line must report
`completion=complete` and zero A/B send failures. `completion=interrupted` or
`completion=source_error` identifies an incomplete replay and returns a
nonzero exit status.

## Replay Modes

### Redundant A/B Skew

`ASTRA_LINE_B_DELAY_NS` controls the deterministic dispatch skew between the
redundant paths. It defaults to `1000` ns: line A is dispatched first and line B
is released 1 microsecond later. Set it to `0` to disable the intentional skew.
Values up to `1000000000` ns (1 second) are accepted.
This is a deterministic test model, not a Nasdaq protocol guarantee: real
redundant-path skew varies and either path can lead. The OS and NIC can also
introduce additional variation in observed arrival times.

The B-line worker busy-polls its preallocated handoff to avoid scheduler wakeup
jitter, so `ASTRA_CPU_B` must be treated as a dedicated sender CPU during a
benchmark.
`line_b_delay_overruns` counts packets for which B observed the handoff only
after the configured deadline; those packets still remain A-first, but their
actual software dispatch skew was larger than requested.

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
the receiver can construct and seal all registered book roots before pre-market
order flow resumes. The large global directory, order arena, and price pools
are already allocated and page-touched at process startup.

### Flat Stress Mode

Use flat mode when you want a deterministic stress window instead of the real
ITCH burst shape.

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
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
ASTRA_LINE_B_DELAY_NS=1000 \
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
sender_stats completion=complete first_seq=1
line_a_send_failures=0
line_b_send_failures=0
channel_first_received_seq=1
line_a_first_received_seq=1
line_b_first_received_seq=1
session_initialized=1
session_mismatch_packets=0
gap_buffer_remaining=0
channel_status_name=Good
line_a_kernel_drops=0
line_b_kernel_drops=0
```

For DPDK, use `imissed=0`, `ierrors=0`, and `rx_nombuf=0` in place of the
kernel-drop fields. After sender completion, wait until receiver
`channel_next_seq` equals sender `next_seq`, then stop the engine. Exact
equality is mandatory for a clean full-stream result. An interrupted sender or
any remaining sequence delta is incomplete and must not be recorded as a clean
latency baseline. Persistent `GapDetected`, a nonzero session mismatch, or any
drop/failure counter also invalidates the run.

The final `book_stats` line is also an acceptance gate. In particular, reject a
run with any nonzero allocation, directory, stale/missing reference, late book,
price-pool exhaustion, invalid-release, mutation-failure, price-rejection, or
locally-invalid-book counter. `md_engine` exits nonzero when the final startup,
session, channel, gap, or book-state gate is not clean.

### Offline full-file correctness benchmark

`astra_itch_replay_benchmark` replays length-prefixed ITCH records directly
through `ItchParser` and the order books. It excludes UDP/DPDK and latency
timestamping, and exits nonzero unless all representation gates are clean.

```bash
cmake -S . -B build-perf \
  -DASTRA_BUILD_TESTS=OFF \
  -DASTRA_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf --target astra_itch_replay_benchmark -j

ASTRA_ORDER_DIRECTORY_SLOTS=4194304 \
./build-perf/benchmarks/astra_itch_replay_benchmark \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50
```

On 2026-07-10, a local ARM64 macOS build using Apple Clang 21 and `-O3`
processed all `368366634` records (`11245883092` bytes) in `86.597 s`, or
`4.254 million records/s`. All failure counters were zero. Measured concurrent
high-watermarks were `1742866` orders, `71065` internal price nodes, `587520`
price leaves, and `707130` occupied side/price levels. This validates the
implementation against that trace; it is not a UDP latency result and does not
replace multi-day or live-capacity testing.

### Latency Measurement Boundary

The latency histogram is recorded by `md_engine` after `MoldUdpDecoder`
processes a packet. For each MoldUDP64 packet, the decoder measures elapsed time
from `PacketView.receive_start_ticks` through MoldUDP decoding, ITCH parsing,
and book updates, then records the packet elapsed time divided by processed ITCH
message count and weights it by that message count.

For regular `recv`, `receive_start_ticks` is stamped immediately before
`recvmsg()`, after the kernel has already handled Ethernet/IP/UDP receive work
outside the user-space timestamp. For DPDK with
`ASTRA_DPDK_LATENCY_MODE=packet`, `receive_start_ticks` is stamped after
`rte_eth_rx_burst()` returns an mbuf and immediately before DPDK frame parsing,
so the reported DPDK latency includes the user-space Ethernet/IPv4/UDP fast-path
parse in addition to the same MoldUDP/ITCH/book path.

## Recent Results

The network-latency rows below are historical and were collected across
different code revisions before the current order-directory, global-order-pool,
and full-price-radix redesign. Keep them as runbook references only. Rerun every
transport on one clean Release/IPO binary and one Git SHA before making a
current performance comparison.

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

### DPDK Timestamp-Shaped Replay

`dpdk`, A/B lines, `20` messages per packet,
`ASTRA_PREMARKET_REPLAY_MODE=timestamp`, `ASTRA_PREMARKET_SPEEDUP=33`,
secondary ENI destination `172.31.32.18`, DPDK port `0000:28:00.0`.

Receiver settings:

```bash
ASTRA_CPU=2 \
ASTRA_NUMA_NODE=0 \
ASTRA_DPDK_PORT_ID=0 \
ASTRA_DPDK_BURST_SIZE=16 \
ASTRA_DPDK_LATENCY_MODE=packet \
ASTRA_DPDK_FLOW_FILTER=off \
ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2 --allow 0000:28:00.0" \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./scripts/run_engine_dpdk.sh 0.0.0.0 9000 0.0.0.0 9001
```

Sender settings:

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
ASTRA_NUMA_NODE=0 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.18 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  100000
```

| Post-`SQ` rate per line | SS pause | DPDK burst | Status | `imissed / ierrors / rx_nombuf` | Fast / fallback path | Final sequence |      p50 |      p99 |    p99.9 |    p99.99 |
| ----------------------: | -------: | ---------: | ------ | ------------------------------: | -------------------: | -------------: | -------: | -------: | -------: | --------: |
|          `100000 pkt/s` |   `30 s` |        `8` | `Good` |                     `0 / 0 / 0` |       `36836680 / 0` |    `368366635` | `304 ns` | `613 ns` | `819 ns` | `1002 ns` |
|          `100000 pkt/s` |  `120 s` |        `8` | `Good` |                     `0 / 0 / 0` |       `36836682 / 0` |    `368366635` | `272 ns` | `493 ns` | `704 ns` |  `825 ns` |
|          `100000 pkt/s` |  `120 s` |       `16` | `Good` |                     `0 / 0 / 0` |       `36836684 / 0` |    `368366635` | `256 ns` | `456 ns` | `690 ns` |  `796 ns` |
|          `100000 pkt/s` |  `120 s` |       `32` | `Good` |                     `0 / 0 / 0` |       `36836679 / 0` |    `368366635` | `273 ns` | `469 ns` | `708 ns` |  `819 ns` |

For the historical EC2 binary and feed shape, DPDK burst `16` was the best
measured setting.
Burst `32` was clean but had worse p99 and tail latency than burst `16`.

Latest clean-run details for the `120 s` / burst `16` row:

```text
sender next_seq=368366635
receiver channel_next_seq=368366635
line_a_packets=18418332
line_b_packets=18418332
filtered=0
malformed=20
imissed=0
ierrors=0
rx_nombuf=0
latency count=368366634
mean_ns=312.94
max_ns=970036147
```

The `max_ns` value in these runs includes the one-time `SS` book creation and
warmup event, so use the percentile distribution and zero-drop validation when
comparing steady-state packet processing.

## Test Environment

```text
Sender: c7i.4xlarge with 100 GB gp3 EBS
Engine: r7iz.8xlarge
Transport: regular UDP recv and DPDK, redundant A/B lines
DPDK feed ENI: 172.31.32.18 / enp40s0 / 0000:28:00.0
```
