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

Use the AWS runbook below for DPDK package installation, engine build, NUMA
and ENI binding, engine startup, and ENI recovery commands.

## Performance

### Full S061226 AWS DPDK run

The complete `S061226-v50.txt` session was replayed on 2026-07-29 from a
`c7i.4xlarge` sender into a dedicated secondary ENI on an `r7i.16xlarge`
receiver. Both hosts reported commit `8651fbc91be3`; the receiver used DPDK
23.11.4, Release, IPO off, CPU 2 on NUMA node 0, one RX queue, 8,192 RX
descriptors, burst 32, packet-latency mode, and 20 messages per packet.

The sender's `100,000` packet/s-per-line argument is a ceiling, not a
guaranteed rate. The observed receiver rate was approximately 28,200 physical
packets/s, or 14,100 packets/s per line and 282,000 logical messages/s. The
sender reported 69,338 line-B delay overruns (about 0.11% of packets), so this
run does not certify a precise 1 us redundant-line skew.

| Test file | Messages | Mean (ns) | Min | p50 | p90 | p99 | p99.9 | p99.99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `S061226-v50.txt` | 1,304,894,064 | 158.02 | 6 | 154 | 202 | 281 | 585 | 678 | 206,180 |

The run reached sequence `1,304,894,065` with channel status `Good`, phase
`EndOfMessages`, the end marker accepted, zero live orders, all 12,809 books
present and consistent, 156,871 of 156,872 price pages committed, and zero
capacity failures. Both lines sent 65,244,715 packets with zero send failures.
The receiver intentionally stopped on the first valid end marker, after
65,244,706 line-A and 65,244,704 line-B packets. It reported zero gaps,
conflicting redundant packets, malformed packets, missed packets, RX errors,
and mbuf exhaustion.

This is a full live-path correctness pass and a live CPU-processing latency
observation. It is not the separate five-process
`run_order_book_acceptance.sh` latency gate, and no absolute live-DPDK p99 or
p99.9 ceiling has been approved. The metric begins after DPDK dequeue and
covers frame parsing, Mold sequencing, ITCH dispatch, and book mutation; it is
not sender-to-book or network latency.

Provenance caveats: the receiver reported a dirty worktree because the
DPDK-23.11 no-IOMMU compatibility and latency-toggle script changes were
uncommitted during deployment; those changes are incorporated here. The
41,662,444,846-byte corpus matched the recorded size and completed with the
exact profiled record count, but its 41.7 GB SHA-256 was not freshly
recomputed on the sender before this run.

### Historical 2019 load sweep

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
- [AWS EC2 DPDK setup and engine runbook](docs/dpdk-aws-ec2-setup.md)
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
