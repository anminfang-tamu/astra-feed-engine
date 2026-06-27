# DPDK Setup on AWS EC2 Linux

This runbook captures the EC2 DPDK receiver setup used for
`astra-feed-engine`. It assumes the normal `recv()` path already works and that
feed traffic arrives on a secondary ENI.

The example values from the current receiver host are:

```text
primary SSH interface: enp39s0 / 172.31.32.91
feed interface:        enp40s0 / 172.31.32.18
feed PCI address:      0000:28:00.0
feed NUMA node:        0
```

Do not bind the primary SSH interface to DPDK. On this host, that means never
binding `enp39s0` / `0000:27:00.0`.

## 1. Verify the Linux UDP Baseline

Before switching to DPDK, verify the kernel UDP receiver is clean:

```bash
cd ~/astra-feed-engine

ASTRA_CPU=2 \
ASTRA_UDP_RX=recv \
ASTRA_UDP_DROP_METRICS=on \
ASTRA_STAGE_LATENCY_METRICS=off \
./scripts/run_engine_udp.sh 0.0.0.0 9000 0.0.0.0 9001
```

The sender should target the secondary ENI private IP:

```text
172.31.32.18
```

## 2. Install DPDK Build Dependencies

Install the DPDK development package and confirm `pkg-config` can find
`libdpdk`:

```bash
sudo apt update
sudo apt install -y dpdk dpdk-dev libdpdk-dev pkg-config

pkg-config --modversion libdpdk
pkg-config --libs libdpdk
```

If CMake fails with `Package 'libdpdk' not found`, the DPDK development package
or `libdpdk.pc` is still missing from the host.

## 3. Build the DPDK Engine

The DPDK build stays separate from the normal UDP build:

```bash
cmake -S . -B build-dpdk \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_ENABLE_DPDK=ON

cmake --build build-dpdk --target md_engine -j"$(nproc)"
```

`scripts/run_engine_dpdk.sh` uses this `build-dpdk/md_engine` binary and sets
`ASTRA_RX=dpdk` for runtime selection.

## 4. Identify the Secondary ENI

Confirm the feed interface and map it to a PCI device:

```bash
FEED_IFACE=enp40s0
FEED_IP=172.31.32.18
FEED_PCI=$(basename "$(readlink -f /sys/class/net/${FEED_IFACE}/device)")
FEED_NUMA=$(cat "/sys/class/net/${FEED_IFACE}/device/numa_node")

echo "iface=${FEED_IFACE} pci=${FEED_PCI} numa=${FEED_NUMA}"
ethtool -i "${FEED_IFACE}"
```

Expected example:

```text
iface=enp40s0 pci=0000:28:00.0 numa=0
driver: ena
bus-info: 0000:28:00.0
```

Double-check the primary interface remains separate:

```bash
ip -br addr
```

## 5. Prepare Hugepages and DPDK Device Binding

Set hugepages and locate `dpdk-devbind.py`:

```bash
sudo sysctl -w vm.nr_hugepages=2048
sudo modprobe vfio-pci

DPDK_DEVBIND=$(command -v dpdk-devbind.py || echo /usr/share/dpdk/usertools/dpdk-devbind.py)
sudo "${DPDK_DEVBIND}" --status
```

On the tested EC2 host, normal VFIO binding failed because IOMMU was disabled:

```text
Error: IOMMU support is disabled, use --noiommu-mode for binding in noiommu mode
```

For this isolated benchmark host, enable VFIO no-IOMMU mode:

```bash
sudo modprobe vfio enable_unsafe_noiommu_mode=1
sudo modprobe vfio-pci

echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

This mode is less isolated than normal VFIO. Use it only on a controlled
benchmark instance.

## 6. Remove Linux Routing From the Feed ENI

Before binding the secondary ENI to DPDK, remove the Linux IP/routing state for
that ENI:

```bash
sudo ip rule del from "${FEED_IP}" table 1001 2>/dev/null || true
sudo ip route flush table 1001 2>/dev/null || true
sudo ip rule del from "${FEED_IP}" table 101 2>/dev/null || true
sudo ip route flush table 101 2>/dev/null || true
sudo ip addr flush dev "${FEED_IFACE}"
sudo ip link set "${FEED_IFACE}" down
```

## 7. Bind the Feed ENI to DPDK

Bind only the secondary ENI PCI address:

```bash
sudo "${DPDK_DEVBIND}" --noiommu-mode --bind=vfio-pci "${FEED_PCI}"
sudo "${DPDK_DEVBIND}" --status
```

Expected status shape:

```text
Network devices using DPDK-compatible driver
============================================
0000:28:00.0 'Elastic Network Adapter (ENA) ec20' numa_node=0 drv=vfio-pci unused=ena

Network devices using kernel driver
===================================
0000:27:00.0 'Elastic Network Adapter (ENA) ec20' numa_node=0 if=enp39s0 drv=ena unused=vfio-pci *Active*
```

After this, `enp40s0` may disappear from `ip -br addr`, and `tcpdump -i
enp40s0` is no longer useful because DPDK owns the NIC.

## 8. Run the DPDK Receiver

Start the receiver through the repo wrapper:

```bash
sudo env \
  ASTRA_CPU=2 \
  ASTRA_NUMA_NODE="${FEED_NUMA}" \
  ASTRA_DPDK_PORT_ID=0 \
  ASTRA_DPDK_BURST_SIZE=16 \
  ASTRA_DPDK_LATENCY_MODE=packet \
  ASTRA_DPDK_FLOW_FILTER=off \
  ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2 --allow ${FEED_PCI}" \
  ASTRA_UDP_DROP_METRICS=on \
  ASTRA_STAGE_LATENCY_METRICS=off \
  ./scripts/run_engine_dpdk.sh 0.0.0.0 9000 0.0.0.0 9001
```

The sender command stays the same as the `recv()` test. It should target:

```text
172.31.32.18
```

DPDK flow filtering is on by default. The receiver enables isolated mode and
installs `rte_flow` rules for destination UDP ports `9000` and `9001`, while
the parser remains in place as a safety net and to preserve A/B line
attribution. If the active PMD rejects `rte_flow_isolate` or the IPv4/UDP flow
rules, rerun with `ASTRA_DPDK_FLOW_FILTER=off` to use the previous userspace
filtering path. The receiver still consumes the same MoldUDP64 payload path
after packet reception.

`ASTRA_DPDK_LATENCY_MODE=packet` timestamps each accepted packet before DPDK
frame parsing. Use `burst` only when intentionally measuring the older
burst-level queueing view. `ASTRA_DPDK_BURST_SIZE=8` is the recommended first
latency comparison point; larger bursts can be useful for throughput tests.

Clean DPDK acceptance requires:

```text
channel_status_name=Good
imissed=0
ierrors=0
rx_nombuf=0
```

The DPDK `rx_stats` line should also show most packets on `fast_path`. A large
`fallback_path` count means packets are using the general parser, usually due to
VLAN tags, IPv4 options, or a non-standard frame shape.

## 9. Restore the ENI to Linux

When the DPDK test is done, return the secondary ENI to the kernel `ena` driver:

```bash
sudo modprobe ena
sudo "${DPDK_DEVBIND}" --bind=ena "${FEED_PCI}"
sudo "${DPDK_DEVBIND}" --status
```

Bring the OS-side secondary ENI setup back:

```bash
./scripts/setup_secondary_eni.sh --iface "${FEED_IFACE}"
```

Verify source routing again:

```bash
ip route get <sender-private-ip> from "${FEED_IP}"
```

Expected output should use the feed interface, for example:

```text
dev enp40s0 table 1001 src 172.31.32.18
```

## Troubleshooting

`Package 'libdpdk' not found`

Install `dpdk-dev` / `libdpdk-dev` and confirm `pkg-config --modversion
libdpdk` works before rerunning CMake.

`IOMMU support is disabled`

Enable VFIO no-IOMMU mode and bind with `--noiommu-mode`, or reboot onto a host
configuration that exposes IOMMU.

`no DPDK Ethernet ports are available`

The feed ENI is not bound to a DPDK-compatible driver, or the DPDK EAL allowlist
does not match the feed PCI address. Recheck `dpdk-devbind.py --status` and
`ASTRA_DPDK_EAL_ARGS`.

Engine starts but sees no packets

Confirm the sender targets the secondary ENI private IP, security groups allow
UDP `9000` and `9001`, and `ASTRA_DPDK_PORT_ID=0` matches the single allowlisted
DPDK port.
