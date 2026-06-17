# astra-feed-engine

AstraFeed is a low-latency C++ market data feed handler for MoldUDP64-wrapped
NASDAQ ITCH 5.0 replay. The primary benchmark setup uses redundant A/B UDP
sender lines feeding one `md_engine` receiver.

## Linux A/B Replay

This runbook assumes:

- engine host: runs `md_engine`
- sender host: runs two `itch_moldudp_sender` processes through
  `scripts/run_itch_ab_senders.sh`
- ITCH file: `data/itch/unzipped/01302019.NASDAQ_ITCH50`
- packet shape: `20` ITCH messages per MoldUDP64 packet

### Receiver

Start the engine first. Keep drop metrics enabled for benchmark validation.

```bash
ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./build/md_engine 0.0.0.0 9000 0.0.0.0 9001
```

`ASTRA_STAGE_LATENCY_METRICS=off` disables the per-stage breakdown. Packet-level
latency remains enabled unless `ASTRA_LATENCY_METRICS=off` is also set.

### Sender

For a realistic pre-market shape without waiting the full 5.5 hours, use
timestamp replay mode. It follows the ITCH timestamps from `SS` through `SQ`,
scaled by `ASTRA_PREMARKET_SPEEDUP`.

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=30 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  10000
```

The final `10000` is the normal packet rate per line outside the timestamp-paced
pre-market window.

## Replay Modes

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
the receiver can create and touch registered order books before pre-market order
flow resumes.

### Flat Stress Mode

Use flat mode when you want a deterministic stress window instead of the real
ITCH burst shape.

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_PREMARKET_SECONDS=600 \
ASTRA_SS_PAUSE_SECONDS=30 \
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
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.91 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  10000 \
  0 \
  30 \
  timestamp \
  33
```

## Validation

A clean run should end with:

```text
channel_status_name=Good
line_a_kernel_drops=0
line_b_kernel_drops=0
```

For a completed sender stream, `sender next_seq` should match receiver
`channel_next_seq`. If the sender is interrupted first, a small tail delta is
normal. Persistent `GapDetected`, nonzero kernel drops, or a large
sender/receiver sequence gap means the run should not be used as a clean latency
baseline.

## Recent Results

### Full-Day Flat Replay

`recv`, A/B lines, `20` messages per packet, `ASTRA_PREMARKET_SECONDS=600`,
`ASTRA_SS_PAUSE_SECONDS=30`.

| Post-`SQ` rate per line | Status | Kernel drops | Final sequence |      p50 |      p99 |     p99.9 |    p99.99 |
| ----------------------: | ------ | -----------: | -------------: | -------: | -------: | --------: | --------: |
|           `10000 pkt/s` | `Good` |      `0 / 0` |    `102437301` | `170 ns` | `570 ns` | `1535 ns` | `1935 ns` |
|           `50000 pkt/s` | `Good` |      `0 / 0` |    `368366635` | `126 ns` | `296 ns` | `1103 ns` | `1695 ns` |

### Timestamp-Shaped Replay

`recv`, A/B lines, `20` messages per packet,
`ASTRA_PREMARKET_REPLAY_MODE=timestamp`, `ASTRA_PREMARKET_SPEEDUP=33`,
`ASTRA_SS_PAUSE_SECONDS=30`, post-`SQ` rate `10000 pkt/s` per line.

| Status | Kernel drops | Final sequence |      p50 |      p99 |    p99.9 |    p99.99 |
| ------ | -----------: | -------------: | -------: | -------: | -------: | --------: |
| `Good` |      `0 / 0` |     `48386301` | `138 ns` | `320 ns` | `899 ns` | `3759 ns` |

The `max_ns` value in these runs includes the one-time `SS` book creation and
warmup event, so use the percentile distribution and zero-drop validation when
comparing steady-state packet processing.

## Test Environment

```text
Sender: c7i.4xlarge with 100 GB gp3 EBS
Engine: r7iz.8xlarge
Transport: regular UDP recv, redundant A/B lines
```
