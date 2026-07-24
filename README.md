# astra-feed-engine

AstraFeed is a single-writer C++20 feed engine for MoldUDP64-wrapped NASDAQ
ITCH 5.0 with synchronized redundant A/B replay. The hot path uses fixed
startup capacity and bounded order/price lookup.

See [the design and acceptance contract](docs/design.md) for architecture and
[the AWS DPDK runbook](docs/dpdk-aws-ec2-setup.md) for ENI, VFIO, hugepage, and
recovery setup.

## Build and test

Use one `build` directory per host. For the sender or kernel-UDP engine:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=OFF \
  -DASTRA_ENABLE_DPDK=OFF \
  -DASTRA_ENABLE_IPO=OFF

cmake --build build --clean-first -j
ctest --test-dir build --output-on-failure
```

For the Linux DPDK receiver:

```bash
pkg-config --modversion libdpdk

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=OFF \
  -DASTRA_ENABLE_DPDK=ON \
  -DASTRA_ENABLE_IPO=OFF

cmake --build build --clean-first -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Run

Start the engine first and wait for `Engine started` before starting the
sender.

Kernel UDP engine:

```bash
ASTRA_RX=udp \
ASTRA_CPU=2 \
ASTRA_NUMA_NODE=0 \
ASTRA_NUMA_MEM_POLICY=membind \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_LATENCY_METRICS=on \
ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-itch-20190130-acceptance-v1 \
ASTRA_BOOK_PREFAULT=on \
./scripts/run_engine_udp.sh \
  0.0.0.0 9000 0.0.0.0 9001
```

DPDK engine, after completing the AWS runbook and defining `FEED_NUMA`,
`FEED_PCI`, and `FEED_IP`:

```bash
sudo env \
  ASTRA_CPU=2 \
  ASTRA_NUMA_NODE="$FEED_NUMA" \
  ASTRA_NUMA_MEM_POLICY=membind \
  ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-itch-20190130-acceptance-v1 \
  ASTRA_BOOK_PREFAULT=on \
  ASTRA_LATENCY_METRICS=on \
  ASTRA_DPDK_PORT_ID=0 \
  ASTRA_DPDK_QUEUE_ID=0 \
  ASTRA_DPDK_BURST_SIZE=8 \
  ASTRA_DPDK_RX_DESC=8192 \
  ASTRA_DPDK_MEMPOOL_SIZE=65535 \
  ASTRA_DPDK_LATENCY_MODE=packet \
  ASTRA_DPDK_FLOW_FILTER=off \
  ASTRA_DPDK_SKIP_BUILD=on \
  ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2 --allow ${FEED_PCI} --huge-dir /mnt/huge --file-prefix astra" \
  ./scripts/run_engine_dpdk.sh \
    "$FEED_IP" 9000 "$FEED_IP" 9001
```

Synchronized A/B sender, on the external sender host:

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
ASTRA_STARTUP_HEARTBEAT_COUNT=100 \
ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS=10 \
ASTRA_PREMARKET_REPLAY_MODE=off \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_sender.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.18 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  100000
```

Change the final value to select the configured packet rate per line. A clean
full replay ends with sender `completion=complete`, zero send failures,
`end_of_session_sent=true`, engine `channel_next_seq=368366635`,
`channel_status_name=Good`, `latency count=368366634`, and zero malformed or
DPDK loss/error counters.

## Performance

These single-run EC2 DPDK observations used clean Release commit
`ea08c29863e95bde693240c7ae011308173e3212`, CPU 2, NUMA node 0, burst 8,
20 messages per data packet, packet latency mode, and IPO off.

| Configured rate per line (packet/s) | Nominal logical messages/s | Mean (ns) | Min (ns) | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) | p99.99 (ns) | Max (ns) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100,000 | 2,000,000 | 129.13 | 5 | 127 | 175 | 231 | 553 | 682 | 9,552 |
| 150,000 | 3,000,000 | 123.74 | 5 | 121 | 167 | 220 | 586 | 689 | 10,186 |
| 200,000 | 4,000,000 | 122.15 | 5 | 120 | 166 | 219 | 611 | 719 | 9,499 |

Every run completed all 368,366,634 messages with a healthy channel and zero
DPDK missed/error/no-buffer counts. The metric is amortized CPU processing time
per newly processed logical message after DPDK dequeue, not network or
end-to-end latency. These are load-sweep observations, not repeated
deterministic acceptance evidence.
