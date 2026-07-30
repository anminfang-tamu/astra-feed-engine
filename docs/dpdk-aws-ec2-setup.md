# AWS EC2 DPDK and NUMA Runbook

This is the final receiver procedure used for the full `S061226-v50.txt`
replay. It covers persistent two-ENI networking, NUMA selection, DPDK
installation and build, safe ENI binding, latency collection, repeated runs,
and restoration.

The numbered DPDK scripts are deliberately fail-closed. Host identities are
not committed as defaults; export them from `build/dpdk-host.env` before every
numbered step.

## Tested topology

| Role | Tested value |
| --- | --- |
| Receiver | AWS EC2 `r7i.16xlarge`, 64 vCPUs, approximately 512 GiB RAM |
| Receiver OS | Ubuntu, kernel `6.17.0-1017-aws`, cgroup v2 |
| Receiver DPDK | 23.11.4, ENA PMD, one RX queue |
| Sender | AWS EC2 `c7i.4xlarge` |
| Protected/SSH NIC | `enp55s0`, `172.31.65.225`, PCI `0000:37:00.0` |
| Feed NIC | `enp56s0`, `172.31.69.249`, PCI `0000:38:00.0` |
| Feed NUMA node | `0` |
| Engine CPU | `2`; SMT sibling `34` |
| Feed endpoints | `172.31.69.249:9000` and `:9001` |
| Capacity profile | `nasdaq-itch-20260612-v1` |

The checked-in S061226 plan maps `388036034560` bytes before normal runtime
overhead. Use a 512 GiB-class receiver and verify that the feed NIC's NUMA node
and the process cgroup have enough available memory. Do not reduce or invent
capacity values to bypass admission.

## Safety model

- Keep SSH and the main default route on a protected primary ENI.
- Dedicate a secondary ENI to feed traffic and bind only its PCI function.
- Never bind an interface until its MAC, IP, PCI address, NUMA node, driver,
  default-route role, and SSH role have all been checked.
- Prefer VFIO with an IOMMU. The tested r7i host exposed no IOMMU group and
  therefore required explicit unsafe no-IOMMU mode. That mode provides no DMA
  isolation and is acceptable only on a dedicated, disposable test receiver.
- DPDK owns the feed NIC while the engine runs. Linux will not show
  `enp56s0` until the restore step rebinds it to ENA.
- Run the receiver and sender in separate foreground terminals for an
  observable certification run.

## 1. Provision AWS resources

Create or select:

1. A receiver with enough RAM for the admitted S061226 plan.
2. A primary management ENI with SSH access.
3. A secondary feed ENI in the receiver's Availability Zone, attached at a
   nonzero device index.
4. A sender with access to the feed ENI private IP.
5. Security-group and network-ACL rules permitting UDP 9000 and 9001 from the
   sender to the feed ENI.

Do not run the receiver-specific Netplan or cloud-init commands on the sender.

Clone the same source revision on both hosts. The recorded full run used:

```text
branch=6-redesign-order-book-data-structure
git_sha=8651fbc91be3
```

After merging, use the selected clean `main` revision and record it with every
run.

## 2. Discover and record receiver identities

Run before changing networking:

```bash
cd ~/astra-feed-engine

git branch --show-current
git rev-parse --short=12 HEAD
git status --short

ip -br addr
ip -4 route show default
ip -o rule show

sudo /usr/bin/dpdk-devbind.py --status
numactl --hardware
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE

for iface in enp55s0 enp56s0; do
  echo "${iface}"
  cat "/sys/class/net/${iface}/address"
  cat "/sys/class/net/${iface}/device/numa_node"
  basename "$(readlink -f "/sys/class/net/${iface}/device")"
done

test -r /sys/fs/cgroup/cgroup.controllers &&
  echo "cgroup v2: PASS"
```

Replace every tested interface, MAC, IP, and PCI value below if the new host
differs.

## 3. Make the two-ENI network persistent

The feed ENI needs its DHCP address while Linux owns it, but it must not
install default routes, DNS routes, NTP routes, or source-policy rules. The
protected NIC remains the only default route.

Back up the current Netplan file:

```bash
sudo cp -a \
  /etc/netplan/50-cloud-init.yaml \
  /etc/netplan/50-cloud-init.yaml.before-astra
```

On the tested receiver, install:

```bash
sudo tee /etc/netplan/50-cloud-init.yaml >/dev/null <<'EOF'
network:
  version: 2
  ethernets:
    enp55s0:
      match:
        macaddress: "16:ff:c3:20:03:39"
      dhcp4: true
      dhcp4-overrides:
        route-metric: 100
      dhcp6: false
      set-name: "enp55s0"

    enp56s0:
      match:
        macaddress: "16:ff:cd:85:75:5b"
      dhcp4: true
      dhcp4-overrides:
        use-routes: false
        use-dns: false
        use-domains: false
        use-ntp: false
        route-metric: 200
      dhcp6: false
      set-name: "enp56s0"
EOF

sudo chmod 600 /etc/netplan/50-cloud-init.yaml
sudo netplan generate
sudo netplan get
```

### Disable cloud-init network rewrites

Disabling ordinary cloud-init network generation was not sufficient on the
tested image: the installed hotplug udev hook regenerated Netplan when ENA was
restored. On a dedicated receiver with a fixed ENI layout, disable both paths:

```bash
sudo tee \
  /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg \
  >/dev/null <<'EOF'
network: {config: disabled}
EOF

sudo chmod 600 \
  /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg

sudo test ! -e \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules.astra-disabled

sudo mv \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules.astra-disabled

sudo ln -s \
  /dev/null \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules

sudo udevadm control --reload-rules
```

This disables automatic cloud-init configuration for future hotplugged ENIs,
so any later ENI change must be managed explicitly.

Keep the current SSH session open, preferably open a second one, and apply:

```bash
sudo netplan try --timeout 120
```

Accept only after the second SSH connection works. Verify:

```bash
ip -br addr
ip -4 route show default
ip -o rule show
ip -o -4 route show table main dev enp56s0
ip -4 route show table 101 2>&1 || true
getent hosts github.com
```

Expected state:

- only `enp55s0` has a default route;
- there is no `from 172.31.69.249 lookup 101` rule;
- `enp56s0` has only its connected `172.31.64.0/20` route;
- table 101 is absent or empty;
- DNS still works through the protected NIC.

## 4. Create the host-local DPDK environment

The `build/` directory is Git-ignored. Store the tested receiver identity
there:

```bash
cd ~/astra-feed-engine
mkdir -p build

tee build/dpdk-host.env >/dev/null <<'EOF'
export ASTRA_FEED_IFACE=enp56s0
export ASTRA_FEED_IP=172.31.69.249
export ASTRA_FEED_PCI=0000:38:00.0
export ASTRA_FEED_NUMA=0
export ASTRA_FEED_DRIVER=ena

export ASTRA_PROTECTED_IFACE=enp55s0
export ASTRA_PROTECTED_PCI=0000:37:00.0

export ASTRA_ENGINE_CPU=2
EOF

chmod 600 build/dpdk-host.env
source build/dpdk-host.env

./scripts/dpdk/dpdk_ec2_config.sh
```

Source this file again in every new receiver shell before running steps 5
through 10.

## 5. Install and build

```bash
cd ~/astra-feed-engine
source build/dpdk-host.env

./scripts/dpdk/dpdk_01_install.sh
./scripts/dpdk/dpdk_02_build.sh
```

Step 2 builds Release `md_engine` with DPDK enabled and IPO off. It validates
the checksum-bound capacity evidence, checks NUMA/cgroup memory, and writes
`build/dpdk-capacity-admission.env`. Later steps reject a changed engine,
capacity manifest, NUMA node, or hugepage count.

## 6. Validate NUMA and CPU placement

```bash
source build/dpdk-host.env

numactl --hardware
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE

cat "/sys/class/net/${ASTRA_FEED_IFACE}/device/numa_node"
cat "/sys/devices/system/cpu/cpu${ASTRA_ENGINE_CPU}/topology/thread_siblings_list"

swapon --show
cat /proc/cmdline
cat /sys/kernel/mm/transparent_hugepage/enabled

systemctl is-active irqbalance.service || true
```

Requirements:

- the engine CPU is online and local to the feed NIC's NUMA node;
- the admitted plan fits that node and the process cgroup;
- the engine CPU and its SMT sibling are otherwise idle;
- swap is not active during the measurement;
- transparent hugepages are `always` or `madvise`;
- management-NIC IRQs do not execute on the engine CPU or its sibling.

The tested host used CPU 2 on node 0, with sibling 34. It did not use kernel
`isolcpus` or `nohz_full`; record those settings rather than claiming
isolation that is not configured.

Stop IRQ rebalancing for the DPDK measurement:

```bash
sudo systemctl stop irqbalance.service
```

## 7. Dry-run and bind the feed ENI

First run the full safety preflight without mutation:

```bash
cd ~/astra-feed-engine
source build/dpdk-host.env

./scripts/dpdk/dpdk_03_bind.sh \
  --dry-run \
  --allow-unsafe-noiommu
```

Omit `--allow-unsafe-noiommu` when the PCI function has an IOMMU group.

On the tested no-IOMMU receiver:

```bash
./scripts/dpdk/dpdk_03_bind.sh \
  --allow-unsafe-noiommu
```

The script enables the kernel VFIO unsafe-mode parameter and binds with the
DPDK 23.11-compatible `dpdk-devbind.py --bind=vfio-pci` interface. It does not
pass the unsupported `--noiommu-mode` tool option.

Verify:

```bash
sudo /usr/bin/dpdk-devbind.py --status
ip -br addr
ip -4 route show default
ip -o rule show

test -e build/dpdk-ec2-state.env &&
  echo "Recovery state: PRESENT"

systemctl is-active irqbalance.service || true
```

Expected:

- `0000:38:00.0` uses `vfio-pci`;
- `enp56s0` is absent from Linux;
- `enp55s0` remains up with the sole default route;
- recovery state is present;
- irqbalance is inactive.

## 8. Run the receiver with latency metrics

The launcher defaults latency metrics to `on`. It also passes
`--huge-unlink=always`, which is appropriate because this engine has no DPDK
secondary process and prevents hugetlbfs backing files from surviving a clean
exit.

Run in the foreground:

```bash
cd ~/astra-feed-engine
source build/dpdk-host.env

ASTRA_LATENCY_METRICS=on \
./scripts/dpdk/dpdk_04_run.sh
```

Wait for:

```text
Engine started ... rx=dpdk ... metrics=on
```

For a correctness-only run, explicitly use
`ASTRA_LATENCY_METRICS=off`. Do not run frequent telemetry polling, `perf`,
or unrelated work on the receiver during a p99.99 measurement.

The latency distribution is amortized CPU processing nanoseconds per newly
processed logical message after `rte_eth_rx_burst()` returns. It covers frame
parsing, Mold validation/sequencing, ITCH dispatch, and book mutation. It does
not measure sender-to-NIC, network, RX-queue residence, or end-to-end latency.

## 9. Run the sender

Build or copy the S061226 corpus to the sender. The recorded evidence is:

```text
path=data/itch/unzipped/S061226-v50.txt
bytes=41662444846
sha256=8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238
records=1304894064
completion=legacy_sc_eof
```

For certification, calculate and retain the corpus hash before the run. Do not
hash the 41.7 GB file concurrently with replay.

After the receiver prints `Engine started`, run in a foreground sender shell:

```bash
cd ~/astra-feed-engine

pgrep -af itch_moldudp_sender || true

ASTRA_SENDER_SKIP_BUILD=on \
ASTRA_BINARYFILE_COMPLETION=legacy-sc-eof \
./scripts/run_sender.sh \
  data/itch/unzipped/S061226-v50.txt \
  172.31.69.249 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  100000
```

The packet-rate argument is an upper bound. On the tested sender, the observed
rate was approximately 14,100 packet/s per line rather than 100,000.

## 10. S061226 pass criteria

Sender:

- `completion=complete`;
- `logical_messages=1304894064`;
- `next_seq=1304894065`;
- both send-failure counts are zero;
- `eos_packets_sent=10`, expected 10;
- `binaryfile_completion=legacy_sc_eof`;
- `end_of_session_sent=true`.

Retain `line_b_delay_overruns` as a sender-scheduling diagnostic. It is not a
failure without an approved redundant-line skew threshold, but a nonzero value
means the run does not certify the configured 1 us skew.

Receiver:

- `channel_next_seq=1304894065`;
- `channel_status_name=Good`;
- `channel_phase_name=EndOfMessages`;
- `final_live_orders=0`;
- all 12,809 registered books are present and consistent;
- `committed_price_pages=156871`;
- `price_page_capacity=156872`;
- no page-capacity failures;
- `end_of_stream_accepted=true`;
- zero malformed, missed, error, and no-mbuf counts;
- `latency count=1304894064` and `invalid=0`.

The receiver stops on the first valid redundant end marker, so its A/B packet
counts can be a few packets below the sender's repeated-EOS totals.

The recorded 2026-07-29 live processing distribution was:

| Mean | Min | p50 | p90 | p99 | p99.9 | p99.99 | Max |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 158.02 ns | 6 ns | 154 ns | 202 ns | 281 ns | 585 ns | 678 ns | 206,180 ns |

This full-path result does not replace the separate five-process
`run_order_book_acceptance.sh` gate.

## 11. Repeat a run

Confirm no DPDK process is alive:

```bash
pgrep -af 'md_engine|dpdk-testpmd' || \
  echo "No DPDK process running"
```

With the current launcher, `--huge-unlink=always` should leave all 2,048 pages
free after a clean exit:

```bash
cat \
  /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages \
  /sys/devices/system/node/node0/hugepages/hugepages-2048kB/free_hugepages
```

After an older binary, testpmd run, crash, or leak, inspect before removing
only the selected prefix:

```bash
sudo find /mnt/huge \
  -maxdepth 1 -type f \
  \( -name 'astramap_*' -o -name 'astra-testpmdmap_*' \) \
  -print
```

Only when no DPDK process has the mount open:

```bash
sudo fuser -vm /mnt/huge 2>&1 || true

sudo find /mnt/huge \
  -maxdepth 1 -type f \
  \( -name 'astramap_*' -o -name 'astra-testpmdmap_*' \) \
  -exec unlink {} \;
```

Start a fresh receiver process for every retained latency run. Do not average
percentiles; compare the worst repeated tail.

## 12. Restore the feed ENI to Linux

After the engine stops:

```bash
cd ~/astra-feed-engine
source build/dpdk-host.env

./scripts/dpdk/dpdk_05_restore.sh
sudo systemctl start irqbalance.service
```

Verify:

```bash
sudo /usr/bin/dpdk-devbind.py --status
ip -br addr
ip -4 route show default
ip -o rule show
ip -4 route show table 101 2>&1 || true

test ! -e build/dpdk-ec2-state.env &&
  echo "Recovery state: CLEARED"

systemctl is-active irqbalance.service
sudo netplan get ethernets.enp56s0.dhcp4-overrides
sudo readlink \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules
```

Expected:

- both PCI functions use ENA;
- both Linux interfaces are up;
- only `enp55s0` has a default route;
- table 101 and its source rule remain absent;
- recovery state is cleared;
- irqbalance is active;
- feed DHCP overrides remain disabled;
- the cloud-init hotplug rule resolves to `/dev/null`.

## Troubleshooting

### Unsupported route on the feed interface

If bind reports a DHCP route such as the VPC DNS host route, set
`use-dns: false` as well as `use-routes: false` on the feed Netplan stanza,
apply with `netplan try`, and rerun the dry-run.

### Routes return after ENA restore

If the feed default route or table 101 returns after a bind/restore cycle,
cloud-init hotplug rewrote Netplan. Confirm both the cloud-init disable file
and `/dev/null` udev-rule mask from step 3.

### Fewer than 2,048 hugepages are free

Stop the owning DPDK process. If no process owns `/mnt/huge`, inspect and
unlink only stale `astramap_*` or `astra-testpmdmap_*` files as shown in
step 11.

### Continuous `Gap meet` output

Stop the sender and receiver, retain the first gap lines plus sender,
`engine_stats`, `gap_stats`, and `rx_stats`, then test the raw ENA path with
one RX queue in `dpdk-testpmd`. Do not treat a requested sender packet ceiling
as achieved throughput; compare sender packet totals with receiver totals.

### Telemetry monitor ends with `BrokenPipeError`

This is expected after a healthy EOS: the engine exits and closes its
telemetry socket.

### Roll back dedicated-host cloud-init changes

Only after the feed ENI is restored to ENA:

```bash
sudo unlink \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules

sudo mv \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules.astra-disabled \
  /etc/udev/rules.d/90-cloud-init-hook-hotplug.rules

sudo udevadm control --reload-rules

sudo mv \
  /etc/netplan/50-cloud-init.yaml.before-astra \
  /etc/netplan/50-cloud-init.yaml

sudo netplan try --timeout 120
```

Remove `99-disable-network-config.cfg` only if cloud-init should resume owning
network configuration on that host.

## References

- [DPDK 23.11 EAL parameters](https://doc.dpdk.org/guides-23.11/linux_gsg/linux_eal_parameters.html)
- [DPDK ENA poll-mode driver](https://doc.dpdk.org/guides-23.11/nics/ena.html)
- [DPDK Linux driver guide](https://doc.dpdk.org/guides-23.11/linux_gsg/linux_drivers.html)
- [Project design and acceptance contract](design.md)
- [S061226 trace manifest](trace-manifest-S061226-v50.txt)
- [S061226 capacity evidence](book-capacity-evidence-S061226-v50.txt)
