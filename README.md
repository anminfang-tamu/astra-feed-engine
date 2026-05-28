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
latency count=1200590 invalid=0 min_ns=43 max_ns=175306683 mean_ns=31789.81 p50_ns=1199 p90_ns=17983 p99_ns=42671 p99.9_ns=927743 p99.99_ns=175306683
```
