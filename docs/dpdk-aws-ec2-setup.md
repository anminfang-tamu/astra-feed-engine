# DPDK Setup on AWS EC2

This runbook does one thing: prepare a branch-6 EC2 receiver for DPDK and
start `md_engine`. It does not run a replay, sender, benchmark, trace
profiler, logger, or run-evidence workflow.

The receiver must be Linux with a unified cgroup v2 hierarchy, use an AWS ENA
secondary ENI dedicated to feed traffic, and receive packets from another
host. Once DPDK owns the secondary ENI, the Linux network stack cannot send
traffic to it from the same instance. Run the commands from Bash; the numbered
scripts use Bash features.

The scripts default to the receiver values already used by this project:

```text
primary SSH interface: enp39s0 / 172.31.32.91
primary PCI address:   0000:27:00.0
feed interface:        enp40s0 / 172.31.32.18
feed PCI address:      0000:28:00.0
feed NUMA node:        0
engine CPU:            2
```

These values are host-specific. Never bind the primary, default-route, or SSH
interface to DPDK. The binding script checks all three identities before it
changes the host.

Branch 6 requires a capacity configuration before the process can initialize
DPDK. `dpdk_04_run.sh` reads the tracked configuration directly from
`docs/book-capacity-evidence-S061226-v50.txt`; it does not generate evidence or
need the source trace. That configuration maps `388036034560` bytes and
prefaulting is enabled by default, so use a 512 GiB-class receiver whose feed
NUMA node has enough memory.

That default is limited to the checked-in S061226 deployment profile; it is
not a generic live-feed or multi-day capacity. For another approved
deployment, override both inputs before running step 2 and keep the same
exports through steps 3 and 4:

```bash
export ASTRA_BOOK_CAPACITY_FILE=/absolute/path/to/capacity.txt
export ASTRA_BOOK_CAPACITY_FILE_SHA256=REPLACE_WITH_64_HEX_SHA256
```

## 0. Check the host values

All numbered scripts share
[`scripts/dpdk/dpdk_ec2_config.sh`](../scripts/dpdk/dpdk_ec2_config.sh). On the recorded
receiver, inspect the defaults:

```bash
cd ~/astra-feed-engine
git switch 6-redesign-order-book-data-structure

source ./scripts/dpdk/dpdk_ec2_config.sh
./scripts/dpdk/dpdk_ec2_config.sh
```

For another instance, edit that file or export the correct values in the
shell that will run the steps:

```bash
export ASTRA_FEED_IFACE=enp40s0
export ASTRA_FEED_IP=172.31.32.18
export ASTRA_FEED_PCI=0000:28:00.0
export ASTRA_FEED_NUMA=0
export ASTRA_PROTECTED_IFACE=enp39s0
export ASTRA_PROTECTED_PCI=0000:27:00.0
export ASTRA_ENGINE_CPU=2
```

Stop if any value is uncertain. Confirm interfaces and CPU/NUMA placement
before binding:

```bash
ip -br addr
ethtool -i "$ASTRA_FEED_IFACE"
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
test -r /sys/fs/cgroup/cgroup.controllers
```

## 1. Install DPDK

The first script installs only the build, networking, NUMA, and DPDK packages
needed by the engine:

```bash
./scripts/dpdk/dpdk_01_install.sh
```

It finishes by checking that `pkg-config` can find `libdpdk` and that
`dpdk-devbind.py` is installed.

## 2. Build the DPDK engine

Build only `md_engine`, with tests and benchmarks disabled:

```bash
./scripts/dpdk/dpdk_02_build.sh
```

The result is `build/md_engine`, configured with
`ASTRA_ENABLE_DPDK:BOOL=ON`. Before this step succeeds, it also asks the engine
for its read-only storage plan and verifies that the selected NUMA node and
the effective cgroup v2 memory/2 MiB-hugetlb limits have enough currently
available capacity. It saves the admitted manifest, binary hash, NUMA node,
hugepage count, and required bytes under `build/`. Step 3 requires an exact
match and repeats the memory check immediately before any host mutation.

## 3. Give the secondary ENI to DPDK

Prefer normal VFIO with IOMMU isolation:

```bash
./scripts/dpdk/dpdk_03_bind.sh
```

Before changing anything, the script verifies:

- the configured feed interface, IP, PCI address, NUMA node, and ENA driver;
- the separately configured protected interface and PCI address;
- every main-table default-route interface and the current SSH route;
- that configured policy tables `1001` and legacy `101` contain only feed-ENI
  routes and simple source rules before touching either table;
- that the feed ENI has no global IPv6 address or non-regenerable route outside
  those saved IPv4 policy tables.

It then reserves 2,048 2 MiB hugepages on the feed NUMA node, mounts
`/mnt/huge`, removes Linux addressing from only the feed ENI, and binds only
the feed PCI device to `vfio-pci`.

To inspect the privileged commands without applying them:

```bash
./scripts/dpdk/dpdk_03_bind.sh --dry-run --allow-unsafe-noiommu
```

The recorded branch-5 host did not expose an IOMMU group, so its dry run also
needs the explicit unsafe-mode acknowledgement shown above. On an IOMMU-enabled
host, omit `--allow-unsafe-noiommu`.

If the script reports that the feed PCI device has no IOMMU group, nothing has
been changed. Prefer an IOMMU-enabled host. On a dedicated, disposable
receiver only, explicitly allow VFIO no-IOMMU mode for the real bind:

```bash
./scripts/dpdk/dpdk_03_bind.sh --allow-unsafe-noiommu
```

No-IOMMU mode removes DMA isolation. Do not use it on a shared or production
host.

## 4. Run the engine

Start the engine:

```bash
./scripts/dpdk/dpdk_04_run.sh
```

The script loads the required branch-6 capacity values, disables latency
metrics, selects DPDK port and queue `0`, allowlists only the feed PCI device,
and starts the two receive endpoints on the feed address:

```text
172.31.32.18:9000
172.31.32.18:9001
```

Successful startup ends with a line shaped like:

```text
Engine started ... rx=dpdk ... dpdk_port=0 dpdk_queue=0 metrics=off
```

The engine is then polling the secondary ENI. An external source compatible
with the selected capacity profile must send UDP to the feed private IP on
ports `9000` and `9001`, and the EC2 security group and network ACL must allow
those packets.

Use Ctrl+C to stop. Branch 6 can return a nonzero status when it is interrupted
before a complete end-of-session sequence; that does not by itself mean DPDK
initialization failed.

## 5. Return the ENI to Linux

After the engine has stopped, restore the ENA driver, address, and source route:

```bash
./scripts/dpdk/dpdk_05_restore.sh
```

The bind step stores a small recovery snapshot under the ignored `build/`
directory. The restore script uses it to rebind only `0000:28:00.0` to `ena`,
restore the saved IPv4 CIDR and policy routes/rules, return the hugepage
settings to their previous values, and restore the prior VFIO no-IOMMU
setting. If binding fails or the script is interrupted, it invokes the same
restore path automatically and retains the snapshot if rollback fails. It
does not create logs or run evidence.

## Troubleshooting

`pkg-config cannot find libdpdk`

Rerun `dpdk_01_install.sh`. On a supported Ubuntu/Debian host,
`pkg-config --modversion libdpdk` must succeed before the build.

`No IOMMU group is available`

Use an IOMMU-enabled instance when possible. The explicit
`--allow-unsafe-noiommu` fallback is only for an isolated host where the loss
of DMA isolation is acceptable.

`no DPDK Ethernet ports are available`

Run `dpdk-devbind.py --status` and confirm the configured feed PCI address is
using `vfio-pci`. Also confirm `ASTRA_FEED_PCI` matches the EAL allowlist shown
by `dpdk_04_run.sh`.

`Cannot allocate memory` or an engine stall during prefault

The checked-in branch-6 capacity configuration is large. Verify memory on the
configured NUMA node and the process cgroup. Do not substitute arbitrary
capacity values merely to bypass startup validation.

For background on the device operations, see the
[DPDK Linux driver guide](https://doc.dpdk.org/guides-25.03/linux_gsg/linux_drivers.html)
and the [DPDK ENA poll-mode driver guide](https://doc.dpdk.org/guides-21.05/nics/ena.html).
