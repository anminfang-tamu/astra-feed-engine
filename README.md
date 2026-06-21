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
./scripts/run_engine_udp.sh
```

`ASTRA_STAGE_LATENCY_METRICS=off` disables the per-stage breakdown. Packet-level
latency remains enabled unless `ASTRA_LATENCY_METRICS=off` is also set.

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
same `ASTRA_NUMA_NODE` and `ASTRA_NUMA_MEM_POLICY` variables; pick
`ASTRA_CPU_A` and `ASTRA_CPU_B` from that node when binding the sender host.

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
ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2" \
./scripts/run_engine_dpdk.sh 0.0.0.0 9000 0.0.0.0 9001
```

DPDK is off by default. The wrapper builds `build-dpdk/md_engine` with
`-DASTRA_ENABLE_DPDK=ON` and runs it with `ASTRA_RX=dpdk`. Validate the host
first with hugepages, NIC binding, PMD availability, and `testpmd` RX.
`ASTRA_DPDK_BURST_SIZE` must be divisible by `8`.
`ASTRA_DPDK_LATENCY_MODE=packet` timestamps each accepted packet before DPDK
frame parsing; use `burst` only when you want the older burst-level queueing
view.

For the full AWS EC2 setup flow, including secondary-ENI binding, VFIO
no-IOMMU mode, and restoring the NIC to Linux, see
`docs/dpdk-aws-ec2-setup.md`.

Clean DPDK acceptance requires:

```text
channel_status_name=Good
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
ASTRA_NUMA_NODE=0 \
ASTRA_PREMARKET_REPLAY_MODE=timestamp \
ASTRA_PREMARKET_SPEEDUP=33 \
ASTRA_SS_PAUSE_SECONDS=30 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.18 \
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
channel_status_name=Good
line_a_kernel_drops=0
line_b_kernel_drops=0
```

For a completed sender stream, `sender next_seq` should match receiver
`channel_next_seq`. If the sender is interrupted first, a small tail delta is
normal. Persistent `GapDetected`, nonzero kernel drops, or a large
sender/receiver sequence gap means the run should not be used as a clean latency
baseline.

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
ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2 --allow 0000:28:00.0" \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./scripts/run_engine_dpdk.sh 0.0.0.0 9000 0.0.0.0 9001
```

Sender settings:

```bash
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
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
|          `100000 pkt/s` |   `30 s` |        `8` | `Good` |                       `0 / 0 / 0` |       `36836680 / 0` |    `368366635` | `304 ns` | `613 ns` | `819 ns` | `1002 ns` |
|          `100000 pkt/s` |  `120 s` |        `8` | `Good` |                       `0 / 0 / 0` |       `36836682 / 0` |    `368366635` | `272 ns` | `493 ns` | `704 ns` |  `825 ns` |
|          `100000 pkt/s` |  `120 s` |       `16` | `Good` |                       `0 / 0 / 0` |       `36836680 / 0` |    `368366635` | `271 ns` | `429 ns` | `693 ns` |  `786 ns` |
|          `100000 pkt/s` |  `120 s` |       `32` | `Good` |                       `0 / 0 / 0` |       `36836679 / 0` |    `368366635` | `273 ns` | `469 ns` | `708 ns` |  `819 ns` |

For this EC2 host and feed shape, DPDK burst `16` is the best current setting.
Burst `32` was clean but had worse p99 and tail latency than burst `16`.

Latest clean-run details for the `120 s` / burst `16` row:

```text
sender next_seq=368366635
receiver channel_next_seq=368366635
line_a_packets=18418332
line_b_packets=18418332
filtered=16
malformed=0
imissed=0
ierrors=0
rx_nombuf=0
latency count=368366634
mean_ns=325.08
max_ns=944172436
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
