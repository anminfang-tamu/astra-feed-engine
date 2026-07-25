# astra-feed-engine

AstraFeed is a single-writer C++20 market-data engine for Nasdaq
TotalView-ITCH 5.0 over MoldUDP64. It merges redundant A/B feeds, validates
session sequencing and the daily ITCH lifecycle, and maintains an independent
aggregate L2 order book for every valid Stock Locate.

The repository also includes a deterministic BinaryFILE sender, trace
profiler, capacity-evidence tooling, benchmarks, and correctness tests.

## Build and test

Requirements: CMake 3.20 or newer, a C++20 compiler, and Python 3.

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_BENCHMARKS=ON

cmake --build build -j
ctest --test-dir build --output-on-failure
```

`ASTRA_BUILD_APPS` and `ASTRA_BUILD_TESTS` are enabled by default. DPDK is
Linux-only and can be enabled with `-DASTRA_ENABLE_DPDK=ON`.

## Run

The live engine requires a checksum-bound capacity manifest generated for the
selected corpus. Do not invent or reuse capacity values from another trace.
The sender and kernel-UDP launcher expose their supported configuration:

```bash
./scripts/run_sender.sh --help
./scripts/run_engine_udp.sh --help
```

Use the AWS runbook below for capacity derivation, NUMA, DPDK, sender, replay,
validation, and recovery commands.

## Performance

Historical single-run EC2 DPDK results from Release commit
`ea08c29863e95bde693240c7ae011308173e3212`, using the 2019 corpus, 20 messages
per packet, CPU 2, NUMA node 0, burst 8, packet-latency mode, and IPO off:

| Packets/s per line | Messages/s | Mean (ns) | Min | p50 | p90 | p99 | p99.9 | p99.99 | Max |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100,000 | 2,000,000 | 129.13 | 5 | 127 | 175 | 231 | 553 | 682 | 9,552 |
| 150,000 | 3,000,000 | 123.74 | 5 | 121 | 167 | 220 | 586 | 689 | 10,186 |
| 200,000 | 4,000,000 | 122.15 | 5 | 120 | 166 | 219 | 611 | 719 | 9,499 |

The metric is amortized CPU processing time per newly processed message after
DPDK dequeue, not network or end-to-end latency. See the design document for
measurement and acceptance details.

## Documentation

- [Design and acceptance contract](docs/design.md)
- [Whole-project correctness review](docs/correctness-review-20260724.md)
- [AWS EC2 and DPDK replay runbook](docs/dpdk-aws-ec2-setup.md)
- 2026 evidence: [trace manifest](docs/trace-manifest-S061226-v50.txt),
  [semantic profile](docs/trace-profile-S061226-v50.txt),
  [capacity evidence](docs/book-capacity-evidence-S061226-v50.txt), and
  [storage plan](docs/book-storage-plan-S061226-v50.txt)
- [Historical full-trace verification](docs/full-trace-replay-verification-20260722.txt)

The large files under `data/itch` are intentionally Git-ignored.

This project reconstructs and queries market-data books; it does not yet
provide a strategy publication pipeline, MoldUDP64 retransmission service, or
complete live-feed monitoring. See the correctness review for the current
production-readiness limits.
