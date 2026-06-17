# astra-feed-engine

AstraFeed: Low-Latency C++ Market Data Feed Handler

## Linux A/B Baseline

Baseline captured on Linux with one `md_engine` process receiving two ITCH A/B
sender processes from a second EC2 instance over UDP.

### Run configuration

```bash
ASTRA_CPU=2 ASTRA_UDP_RX=recv ASTRA_STAGE_LATENCY_METRICS=off \
  ./build/md_engine 0.0.0.0 9000 0.0.0.0 9001

ASTRA_CPU=2 ASTRA_UDP_RX=recv ASTRA_UDP_DROP_METRICS=on ASTRA_STAGE_LATENCY_METRICS=off \
  ./build/md_engine 0.0.0.0 9000 0.0.0.0 9001

ASTRA_CPU=2 ASTRA_UDP_RX=recv ASTRA_UDP_DROP_METRICS=on ASTRA_STAGE_LATENCY_METRICS=off \
  ./build/md_engine 0.0.0.0 9000 0.0.0.0 9001
```

For packet-level latency only, leave `ASTRA_LATENCY_METRICS=on` and set
`ASTRA_STAGE_LATENCY_METRICS=off`. That records receive timestamp to completed
packet processing/order-book update without collecting the per-stage breakdown.

```bash
ASTRA_CPU_A=3 ASTRA_CPU_B=4 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=30 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 9000 9001 20 "ASTRA     " 10000
```

```bash
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 9000 9001 20 "ASTRA     " 10000 0 30 timestamp 33
```

```
Speedup Examples:
10  -> about 33 minutes
33  -> about 10 minutes
165 -> about 2 minutes
```

Sender rate: `10000 pkt/s` per line, `20` ITCH messages per packet.
For pre-market replay, `ASTRA_PREMARKET_REPLAY_MODE=timestamp` follows the ITCH
timestamps between `SS` (start of system hours) and `SQ` (start of market hours)
scaled by `ASTRA_PREMARKET_SPEEDUP`. For this sample file, `33` compresses the
real 5.5-hour pre-market window to about 10 minutes while preserving its burst
shape. `ASTRA_SS_PAUSE_SECONDS` adds a quiet pause immediately after sending
`SS` so the receiver can finish book creation before pre-market order flow
resumes.

For a flat stress replay instead, set `ASTRA_PREMARKET_SECONDS`, or pass it as
the eighth argument. A ninth argument controls the post-`SS` pause, the tenth
argument controls replay mode, and the eleventh controls timestamp speedup.

### Health checks

```text
symbols=8713
rx=recv
stage_metrics=off
line_a_packets=600293 line_b_packets=600297
```

### Latency baseline

```text
1. Log and time tracing
latency count=1200590 invalid=0 min_ns=43 max_ns=175306683 mean_ns=31789.81 p50_ns=1199 p90_ns=17983 p99_ns=42671 p99.9_ns=927743 p99.99_ns=175306683
2. No log and time tracing
latency count=1200209 invalid=0 min_ns=36 max_ns=216690863 mean_ns=24676.08 p50_ns=974 p90_ns=14623 p99_ns=36575 p99.9_ns=1513471 p99.99_ns=216690863
3. From recv to parsing (5000 packet/sec)
latency count=4656400 invalid=0 min_ns=58 max_ns=218098 mean_ns=914.96 p50_ns=516 p90_ns=716 p99_ns=2655 p99.9_ns=96255 p99.99_ns=162815
4. Pause after SS and pre-allocate order books
latency count=9049160 invalid=0 min_ns=23 max_ns=784023638 mean_ns=1898.53 p50_ns=148 p90_ns=230 p99_ns=514 p99.9_ns=1583 p99.99_ns=2015
5. Pause after SS and pre-allocate order books(10000 packets/sec)
latency count=8177460 invalid=0 min_ns=21 max_ns=785217862 mean_ns=2090.31 p50_ns=152 p90_ns=229 p99_ns=546 p99.9_ns=1599 p99.99_ns=1951
6. Real world Simulation(10000 packets/sec)
symbols=8713
engine_stats channel_next_seq=102437301 channel_status=1 channel_status_name=Good
rx_stats line_a_packets=5121865 line_b_packets=5121865 line_a_errors=0 line_b_errors=0 line_a_truncated=0 line_b_truncated=0 drop_metrics=on line_a_kernel_drops=0 line_b_kernel_drops=0
latency count=102437300 invalid=0 min_ns=32 max_ns=784255010 mean_ns=341.41 p50_ns=170 p90_ns=238 p99_ns=570 p99.9_ns=1535 p99.99_ns=1935
7. Real world Simulation(50000 packets/sec)
symbols=8713
engine_stats channel_next_seq=368366635 channel_status=1 channel_status_name=Good
rx_stats line_a_packets=18418332 line_b_packets=18418332 line_a_errors=0 line_b_errors=0 line_a_truncated=0 line_b_truncated=0 drop_metrics=on line_a_kernel_drops=0 line_b_kernel_drops=0
latency count=368366634 invalid=0 min_ns=19 max_ns=785261060 mean_ns=180.31 p50_ns=126 p90_ns=188 p99_ns=296 p99.9_ns=1103 p99.99_ns=1695

```

### TEST ENV

```text
1. Sender: c7i.4xlarge with 100GB gp3 EBS
2. Engine: r7iz.8xlarge

```
