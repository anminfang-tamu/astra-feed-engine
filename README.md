# astra-feed-engine

AstraFeed: Low-Latency C++ Market Data Feed Handler

## Linux A/B Baseline

Baseline captured on Linux with one `md_engine` process receiving two ITCH A/B
sender processes from a second EC2 instance over UDP.

### Run configuration

```bash
ASTRA_R_BOOK_WARMUP=prepare ASTRA_CPU=2 ASTRA_UDP_RX=recv ASTRA_STAGE_LATENCY_METRICS=off \
  ./build/md_engine 0.0.0.0 9000 0.0.0.0 9001
```

For packet-level latency only, leave `ASTRA_LATENCY_METRICS=on` and set
`ASTRA_STAGE_LATENCY_METRICS=off`. That records receive timestamp to completed
packet processing/order-book update without collecting the per-stage breakdown.

```bash
ASTRA_CPU_A=3 ASTRA_CPU_B=4 ./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.72.10 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  10000
```

Sender rate: `10000 pkt/s` per line, `20` ITCH messages per packet.

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
a. latency count=1200209 invalid=0 min_ns=36 max_ns=216690863 mean_ns=24676.08 p50_ns=974 p90_ns=14623 p99_ns=36575 p99.9_ns=1513471 p99.99_ns=216690863
b. latency count=1202509 invalid=0 min_ns=32 max_ns=184490022 mean_ns=96442.53 p50_ns=748 p90_ns=10287 p99_ns=30991 p99.9_ns=184490022 p99.99_ns=184490022
3. Increased Symbol Capacity
a. latency count=2033663 invalid=0 min_ns=30 max_ns=452364 mean_ns=103.21 p50_ns=37 p90_ns=63 p99_ns=416 p99.9_ns=6975 p99.99_ns=119807 (10000 msg/sec)
b. latency count=8444692 invalid=0 min_ns=27 max_ns=471583 mean_ns=68.76 p50_ns=47 p90_ns=67 p99_ns=244 p99.9_ns=1167 p99.99_ns=27391 (20000 msg/sec)
c. latency count=9999711 invalid=0 min_ns=26 max_ns=472197 mean_ns=55.36 p50_ns=43 p90_ns=51 p99_ns=95 p99.9_ns=552 p99.99_ns=20943 (30000 msg/sec)
d. latency count=13554187 invalid=0 min_ns=26 max_ns=463352 mean_ns=52.47 p50_ns=39 p90_ns=50 p99_ns=83 p99.9_ns=487 p99.99_ns=13023 (50000 msg/sec)
```
