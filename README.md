# astra-feed-engine

AstraFeed: Low-Latency C++ Market Data Feed Handler

## Linux A/B Baseline

Baseline captured on Linux with one `md_engine` process receiving two local
ITCH A/B sender processes over UDP loopback.

### Run configuration

```bash
ASTRA_R_BOOK_WARMUP=prepare ASTRA_CPU=2 ASTRA_UDP_RX=recvmmsg ASTRA_UDP_BATCH_SIZE=64 \
 ./build/md_engine 127.0.0.1 9000 127.0.0.1 9001
```

For packet-level latency only, leave `ASTRA_LATENCY_METRICS=on` and set
`ASTRA_STAGE_LATENCY_METRICS=off`. That records receive timestamp to completed
packet processing/order-book update without collecting the per-stage breakdown.

```bash
ASTRA_CPU_A=3 ASTRA_CPU_B=4 ./scripts/run_itch_ab_senders.sh
```

Sender rate: `5000 pkt/s` per line, `20` ITCH messages per packet.

### Health checks

```text
symbols=8713
line_a_packets=562812 line_b_packets=562812
line_a_kernel_drops=0 line_b_kernel_drops=0
gap_packets=0
sequenced_packets=562812 old_packets=562812
mean_msgs_per_packet=10.00
```

### Latency baseline

```text
latency count=1125624 invalid=0 min_ns=145 max_ns=258565 mean_ns=6205.72 p50_ns=2735 p90_ns=13567 p99_ns=37295 p99.9_ns=68607 p99.99_ns=193535

stage recv_done_to_decode_start count=1125624 invalid=0 min_ns=35 max_ns=24172 mean_ns=51.99 p50_ns=47 p90_ns=56 p99_ns=146 p99.9_ns=151 p99.99_ns=171
stage mold_header_framing count=1125624 invalid=0 min_ns=38 max_ns=6219 mean_ns=378.97 p50_ns=156 p90_ns=731 p99_ns=770 p99.9_ns=831 p99.99_ns=962
stage itch_parse_state count=1125624 invalid=0 min_ns=0 max_ns=255872 mean_ns=1992.47 p50_ns=0 p90_ns=4575 p99_ns=6367 p99.9_ns=13903 p99.99_ns=191487
stage book_update count=1125624 invalid=0 min_ns=0 max_ns=149872 mean_ns=2963.89 p50_ns=0 p90_ns=6671 p99_ns=29055 p99.9_ns=53247 p99.99_ns=82943
stage engine_other count=1125624 invalid=0 min_ns=65 max_ns=26903 mean_ns=818.41 p50_ns=268 p90_ns=1615 p99_ns=1807 p99.9_ns=1919 p99.99_ns=2111
```

Per-message baseline:

```text
itch_parse_state_mean_ns=199.25
book_update_mean_ns=334.92
```
