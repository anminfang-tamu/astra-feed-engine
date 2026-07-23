# DPDK setup for branch 6 on AWS EC2

This runbook restores the EC2 DPDK procedure for
`6-redesign-order-book-data-structure`. It is intentionally branch-6-only.
The receiver needs a secondary ENI dedicated to feed traffic and an external
sender host; Linux loopback traffic cannot reach a DPDK-owned ENI.

Known receiver-host values:

```text
primary SSH interface: enp39s0 / 172.31.32.91
feed interface:        enp40s0 / 172.31.32.18
feed PCI address:      0000:28:00.0
feed NUMA node:        0
```

Never bind the primary SSH device (`enp39s0`, PCI `0000:27:00.0`) to DPDK.
Doing so disconnects the instance.

The current host has one NUMA node, 16 physical cores, and two SMT threads per
core. CPU 2 and CPU 18 are siblings. Use CPU 2 for the engine and leave CPU 18
idle during the run.

## 1. Verify branch, trace, and memory

```bash
cd ~/astra-feed-engine
git switch 6-redesign-order-book-data-structure
git status --short --branch

command -v numactl >/dev/null || {
  sudo apt-get update
  sudo apt-get install -y numactl
}

TRACE="$PWD/data/itch/unzipped/01302019.NASDAQ_ITCH50"
test "$(sha256sum "$TRACE" | awk '{print $1}')" = \
  1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3

lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
numactl --hardware
```

The pinned branch-6 profile maps 196,062,740,480 bytes (182.598 GiB) for book
storage. The selected node/cgroup must cover at least 213,242,609,664 bytes
when the normal 16 GiB reserve is included. DPDK hugepages and the OS require
additional memory. The shown single-node host has enough free memory, but
recheck immediately before every run:

```bash
grep -E 'MemAvailable|SwapTotal' /proc/meminfo
numactl --hardware
```

For a deterministic run, disable swap, use the performance governor when the
host exposes it, enable transparent huge pages in `madvise` or `always` mode,
and keep CPU 2 isolated from other work and IRQs. These are host-wide policy
changes; apply them only on a dedicated benchmark instance.

## 2. Install build and DPDK packages

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git ninja-build pkg-config zlib1g-dev \
  numactl ethtool dpdk dpdk-dev libdpdk-dev

pkg-config --modversion libdpdk
pkg-config --libs libdpdk
```

## 3. Identify the secondary ENI

```bash
FEED_IFACE=enp40s0
FEED_IP=172.31.32.18
FEED_PCI="$(basename "$(readlink -f "/sys/class/net/${FEED_IFACE}/device")")"
FEED_NUMA="$(cat "/sys/class/net/${FEED_IFACE}/device/numa_node")"

printf 'feed_iface=%s feed_ip=%s feed_pci=%s feed_numa=%s\n' \
  "$FEED_IFACE" "$FEED_IP" "$FEED_PCI" "$FEED_NUMA"
ethtool -i "$FEED_IFACE"
ip -br addr
```

Expected values are `0000:28:00.0`, driver `ena`, and NUMA node `0`. Stop if
the resolved PCI address is `0000:27:00.0` or belongs to the SSH interface.

## 4. Build and inspect the branch-6 storage plan

Both EC2 roles use one build directory named `build` on their respective
hosts. The receiver configures its `build` with DPDK enabled; the external
sender later configures its own `build` with DPDK disabled. Do not force a
different generator when reusing an existing `build` directory:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=OFF \
  -DASTRA_ENABLE_DPDK=ON

cmake --build build --clean-first -j"$(nproc)"
ctest --test-dir build --output-on-failure

ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-itch-20190130-acceptance-v1 \
  ./build/md_engine --book-storage-plan-only
```

The last command must report
`planned_storage_bytes=196062740480`.

If `build` already uses Unix Makefiles, omit `-G Ninja`. If it already uses
Ninja, omitting `-G` also preserves Ninja. Delete and recreate generated build
directories only when intentionally changing generators.

## 5. Reserve DPDK hugepages

The branch-6 arenas use anonymous transparent huge pages. DPDK separately
needs hugetlb pages for its mbuf pool:

```bash
sudo mkdir -p /mnt/huge
mountpoint -q /mnt/huge ||
  sudo mount -t hugetlbfs -o pagesize=2M nodev /mnt/huge
sudo sysctl -w vm.nr_hugepages=2048
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo
```

This reserves 4 GiB of 2 MiB pages. Do it before prefaulting the branch-6
arenas.

## 6. Remove Linux ownership and bind only the feed ENI

Locate the DPDK binding tool:

```bash
DPDK_DEVBIND="$(command -v dpdk-devbind.py ||
  printf '%s\n' /usr/share/dpdk/usertools/dpdk-devbind.py)"
test -x "$DPDK_DEVBIND"
sudo "$DPDK_DEVBIND" --status
```

Remove policy routing and addresses only from the secondary ENI:

```bash
sudo ip rule del from "$FEED_IP" table 1001 2>/dev/null || true
sudo ip route flush table 1001 2>/dev/null || true
sudo ip rule del from "$FEED_IP" table 101 2>/dev/null || true
sudo ip route flush table 101 2>/dev/null || true
sudo ip addr flush dev "$FEED_IFACE"
sudo ip link set "$FEED_IFACE" down
```

Try normal VFIO/IOMMU isolation first:

```bash
sudo modprobe vfio-pci
sudo "$DPDK_DEVBIND" --bind=vfio-pci "$FEED_PCI"
```

The tested EC2 host reports:

```text
Error: IOMMU support is disabled, use --noiommu-mode for binding in noiommu mode
```

On this dedicated benchmark instance, enable unsafe no-IOMMU mode and repeat
the binding with `--noiommu-mode`:

```bash
sudo modprobe vfio enable_unsafe_noiommu_mode=1
sudo modprobe vfio-pci
printf '1\n' |
  sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
sudo "$DPDK_DEVBIND" --noiommu-mode --bind=vfio-pci "$FEED_PCI"
```

No-IOMMU VFIO removes DMA isolation. Never use it on a shared or production
host.

Confirm that only the secondary ENI moved:

```bash
sudo "$DPDK_DEVBIND" --status
ip -br addr
```

The successful tested state is:

```text
DPDK-compatible:
  0000:28:00.0 drv=vfio-pci unused=ena

Kernel:
  0000:27:00.0 if=enp39s0 drv=ena *Active*
```

`enp40s0` no longer appears in `ip -br addr` while DPDK owns it. Stop if
`enp39s0` disappears or `0000:27:00.0` is no longer using `ena`.

## 7. Start the branch-6 DPDK engine

Start the engine first:

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

`ASTRA_DPDK_FLOW_FILTER=off` is intentional for the tested AWS ENA PMD. The
branch-6 userspace parser still validates Ethernet/VLAN, IPv4, fragmentation,
UDP lengths, destination address, and destination port before accepting a
payload. Start with burst size 8 for latency; test larger bursts separately for
throughput. In `packet` mode, the latency sample starts after
`rte_eth_rx_burst` returns and includes frame parsing plus ITCH decode/book
mutation. It is a branch-6 processing-hot-path measurement, not kernel-versus-
DPDK transport timing.

Successful initialization on the tested host includes:

```text
EAL: Detected CPU lcores: 32
EAL: Detected NUMA nodes: 1
EAL: Selected IOVA mode 'PA'
EAL: Using IOMMU type 8 (No-IOMMU)
cpu_affinity status=applied cpu=2 phase=post_dpdk_eal
planned_storage_bytes=196062740480
```

After `book_storage_plan`, the process maps and prefaults approximately
182.598 GiB on NUMA node 0. A long period without new output at that point is
expected. Do not interrupt the engine or start the sender until both
`book_storage ...` and `Engine started ...` appear.

## 8. Start the external sender

Run this on the sender EC2 instance, not on the DPDK receiver host. Security
groups and network ACLs must allow UDP ports 9000 and 9001 to `172.31.32.18`.
The synchronized feeder reads one source stream and sends the exact same
MoldUDP64 packet to line A and line B before advancing:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git zlib1g-dev numactl

cd ~/astra-feed-engine
git switch 6-redesign-order-book-data-structure

TRACE="$PWD/data/itch/unzipped/01302019.NASDAQ_ITCH50"
test "$(sha256sum "$TRACE" | awk '{print $1}')" = \
  1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=OFF \
  -DASTRA_ENABLE_DPDK=OFF

cmake --build build --clean-first -j"$(nproc)"
ctest --test-dir build --output-on-failure

ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
ASTRA_STARTUP_HEARTBEAT_COUNT=100 \
ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS=10 \
ASTRA_PREMARKET_REPLAY_MODE=off \
ASTRA_SS_PAUSE_SECONDS=120 \
./scripts/run_itch_ab_senders.sh \
  ./data/itch/unzipped/01302019.NASDAQ_ITCH50 \
  172.31.32.18 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  50000
```

The one-second Mold heartbeat preamble repeatedly advertises next sequence
`1` without advancing the stream. It warms the sender's route/neighbor path
and binds the receiver to the session before the first sequenced ITCH packet.
The heartbeat samples are excluded from latency statistics. Keep the preamble
enabled for EC2 acceptance; its count and interval are explicit so a different
network can tune them without changing code.

The full trace exercises the late Stock Directory `R` after `SS` and order
cleanup after system-event `E`. Branch 6 keeps those paths active; only
system-event `C` is terminal.

## 9. Validate the result

A clean full-trace run has:

```text
sender_stats completion=complete
line_a_send_failures=0
line_b_send_failures=0
startup_heartbeats_sent=100
first_seq=1
next_seq=368366635
end_of_session_sent=true
channel_next_seq=368366635
channel_status_name=Good
malformed=0
imissed=0
ierrors=0
rx_nombuf=0
```

With `ASTRA_DPDK_FLOW_FILTER=off`, ordinary untagged IPv4/UDP feed frames still
use the independently validated software fast parser. `fast_path` should
therefore dominate; `fallback_path` is reserved for VLAN or IPv4-option
frames and unrelated non-fast-path traffic.

Both processes must exit with status zero. Retain the complete sender and
engine logs. Any gap, malformed packet, capacity failure, DPDK
miss/error/no-buffer count, sender failure, or missing EOS marker invalidates
the run. Do not treat the first process as final evidence; repeat with
identical host state and retain every result.

## 10. Return the ENI to Linux

```bash
sudo modprobe ena
sudo "$DPDK_DEVBIND" --bind=ena "$FEED_PCI"
sudo "$DPDK_DEVBIND" --status

./scripts/setup_secondary_eni.sh --iface "$FEED_IFACE"
ip -br addr show dev "$FEED_IFACE"
```

Verify the secondary address and source route before reusing the kernel path.

## Same-host UDP smoke test

For a quick one-instance lifecycle smoke test, do not bind the ENI to DPDK.
Use `127.0.0.1:9000/9001` with the Release UDP binary. This validates parsing
and book behavior but is not DPDK or deterministic EC2 latency evidence.

## Troubleshooting

- `Package 'libdpdk' not found`: install `libdpdk-dev` and verify
  `pkg-config --modversion libdpdk`.
- `generator Ninja does not match Unix Makefiles`: omit `-G Ninja` when
  reusing `build`, or recreate `build` before intentionally changing
  generators.
- `Gap meet channel_expected_seq=1 packet_first_seq=...`: the first sequenced
  datagram arrived before sequence 1. Stop both processes, retain both logs,
  and rerun with the startup heartbeat preamble enabled. The decoder never
  infers a new starting sequence from an arbitrary first datagram.
- `IOMMU support is disabled`: on a dedicated benchmark host only, use the
  documented unsafe VFIO no-IOMMU procedure and bind with `--noiommu-mode`.
- `no DPDK Ethernet ports are available`: check the VFIO binding and that the
  EAL allowlist exactly matches `$FEED_PCI`.
- Engine receives nothing: verify the sender targets `172.31.32.18`, both UDP
  ports are allowed, and the secondary ENI is the only allowlisted device.
- `Cannot allocate memory`: recheck free node/cgroup memory, DPDK hugepages,
  swap/THP policy, and the branch-6 storage plan before starting the process.
- CPU affinity failure: CPU 2 must be online and permitted by the service or
  shell cgroup. Leave sibling CPU 18 idle.
