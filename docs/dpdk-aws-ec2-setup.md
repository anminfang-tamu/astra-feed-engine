# DPDK correctness replay on AWS EC2

This runbook runs the current engine against the checksum-pinned
[official Nasdaq TotalView-ITCH 5.0 file](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/S061226-v50.txt.gz)
for 2026-06-12. The receiver needs a secondary ENI dedicated to feed traffic
and an external sender host; Linux loopback traffic cannot reach a DPDK-owned
ENI.

The procedure certifies clean-trace reconstruction, parser/order-book
semantics, A/B sequence handling, and the declared capacity plan for exactly
those bytes. It does not certify live Nasdaq production readiness. The current
project has no MoldUDP64 re-request/retransmission path, no heartbeat/inactivity
watchdog, no independent per-line liveness/divergence state after A/B merge,
no comparison of already-processed late duplicates or processed prefixes of
differently packetized overlaps, and no IPv4/UDP checksum enforcement in the
DPDK frame parser. The engine has book query objects but no
strategy callback/publication pipeline. Any future strategy integration must
gate trading on `ChannelHealth::Good`. BinaryFILE retains ITCH records, not
original MoldUDP64 packet boundaries, session, multicast addressing, arrival
timing, or A/B skew; this EC2 sender therefore creates a wire-valid but
synthetic unicast transport envelope.

Recorded values for the specific receiver host used by this runbook:

```text
primary SSH interface: enp39s0 / 172.31.32.91
feed interface:        enp40s0 / 172.31.32.18
feed PCI address:      0000:28:00.0
feed NUMA node:        0
```

Never bind the primary SSH device (`enp39s0`, PCI `0000:27:00.0`) to DPDK.
Doing so disconnects the instance.

These addresses, interface names, PCI functions, NUMA node, and CPU IDs are
host-specific operational fixture values, not application constants. On a
different EC2 instance, rediscover them first and deliberately replace the
assertions below; never copy the feed/SSH PCI mapping by assumption.

Any previous approximately 256 GiB receiver is insufficient for this 2026
plan. Use at least 512 GiB total RAM only when the selected NUMA node and
cgroup individually satisfy the derived requirement; aggregate host RAM is
not sufficient when it is split across nonqualifying NUMA nodes.

The large `data/itch` files are intentionally Git-ignored. From the workstation
that holds the download, copy the compressed trace separately to every EC2 host
that will profile or send it:

```bash
set -euo pipefail

ssh ubuntu@EC2_HOST \
  'mkdir -p ~/astra-feed-engine/data/itch/raw ~/astra-feed-engine/data/itch/unzipped'
rsync -ah --info=progress2 \
  data/itch/raw/S061226-v50.txt.gz \
  ubuntu@EC2_HOST:~/astra-feed-engine/data/itch/raw/
ssh ubuntu@EC2_HOST \
  'set -eu
   cd ~/astra-feed-engine
   compressed=data/itch/raw/S061226-v50.txt.gz
   target=data/itch/unzipped/S061226-v50.txt
   test "$(stat -c %s "$compressed")" = 17894268560
   sha256sum "$compressed" |
     grep -q "^1f9d35e12120ef37ea22df9f59dc5207fe51b075b6ccf1c9cdfd09fa566dc1d5  "
   gzip -t "$compressed"
   if [ ! -e "$target" ]; then
     temporary="${target}.partial.$$"
     trap '\''rm -f "$temporary"'\'' EXIT HUP INT TERM
     gzip -dc "$compressed" > "$temporary"
     test "$(stat -c %s "$temporary")" = 41662444846
     sha256sum "$temporary" |
       grep -q "^8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238  "
     mv "$temporary" "$target"
     trap - EXIT HUP INT TERM
   fi
   test "$(stat -c %s "$target")" = 41662444846
   sha256sum "$target" |
     grep -q "^8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238  "'
```

The decompression uses a verified temporary file and publishes it with one
rename, so an interruption cannot leave a partial file at the canonical path.
The tracked corpus manifest remains available through Git.

The recorded host has one NUMA node, 16 physical cores, and two SMT threads per
core. CPU 2 and CPU 18 are siblings there. Use CPU 2 for this recorded setup
and leave CPU 18 idle; rediscover sibling topology before selecting CPUs on
another host.

## 1. Verify revision and trace

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
REVISION_EVIDENCE="$RUN_EVIDENCE/revision"
mkdir -p "$REVISION_EVIDENCE"
git rev-parse HEAD | tee "$REVISION_EVIDENCE/git-head.txt"
git status --porcelain=v1 > "$REVISION_EVIDENCE/git-status.txt"
git diff --binary HEAD > "$REVISION_EVIDENCE/worktree.patch"
if test -s "$REVISION_EVIDENCE/git-status.txt"; then
  echo "acceptance requires the exact reviewed source with no untracked or modified files" >&2
  exit 1
fi
cmake --version > "$REVISION_EVIDENCE/cmake-version.txt"
"${CXX:-c++}" --version > "$REVISION_EVIDENCE/compiler-version.txt"

command -v numactl >/dev/null || {
  sudo apt-get update
  sudo apt-get install -y numactl
}

BINARYFILE_RELATIVE_PATH="data/itch/unzipped/S061226-v50.txt"
TRACE="$PWD/$BINARYFILE_RELATIVE_PATH"
COMPRESSED_TRACE="$PWD/data/itch/raw/S061226-v50.txt.gz"
CORPUS_MANIFEST="$PWD/docs/trace-manifest-S061226-v50.txt"

test "$(stat -c %s "$COMPRESSED_TRACE")" = 17894268560
test "$(sha256sum "$COMPRESSED_TRACE" | awk '{print $1}')" = \
  1f9d35e12120ef37ea22df9f59dc5207fe51b075b6ccf1c9cdfd09fa566dc1d5
gzip -t "$COMPRESSED_TRACE"

test "$(stat -c %s "$TRACE")" = 41662444846
test "$(sha256sum "$TRACE" | awk '{print $1}')" = \
  8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238
test -f "$CORPUS_MANIFEST"

lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
numactl --hardware
```

The manifest records System Event `O` first, System Event `C` final, and no
BinaryFILE zero-length terminator. Strict completion is still the default and
must reject physical EOF. Only the exact SHA-256 above may opt into the
explicit SC-plus-EOF compatibility setting used later.

Do not reuse the pinned 2019 book capacities. Section 4 derives a new custom
profile and prints the exact storage plan. The selected NUMA node and cgroup
must cover that plan, the normal 16 GiB reserve, DPDK hugepages, the OS, and
operational headroom. The completed local book plan is 388,036,034,560 bytes.
Plan plus 16 GiB is 405,215,903,744 bytes; including the documented 4 GiB DPDK
hugepage reserve is 409,510,871,040 bytes before the OS and operational
headroom. The regenerated target Linux plan remains authoritative. Recheck
immediately before every run:

```bash
set -euo pipefail

grep -E 'MemAvailable|SwapTotal' /proc/meminfo
numactl --hardware
```

For a deterministic run, disable swap, use the performance governor when the
host exposes it, enable transparent huge pages in `madvise` or `always` mode,
and keep CPU 2 isolated from other work and IRQs. These are host-wide policy
changes; apply them only on a dedicated benchmark instance.

## 2. Install build and DPDK packages

```bash
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git ninja-build pkg-config python3 \
  numactl ethtool dpdk dpdk-dev libdpdk-dev

pkg-config --modversion libdpdk
pkg-config --libs libdpdk
```

## 3. Identify the secondary ENI

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
mkdir -p "$RUN_EVIDENCE"

FEED_IFACE=enp40s0
FEED_IP=172.31.32.18
FEED_PCI="$(basename "$(readlink -f "/sys/class/net/${FEED_IFACE}/device")")"
FEED_NUMA="$(cat "/sys/class/net/${FEED_IFACE}/device/numa_node")"
FEED_DRIVER="$(basename "$(readlink -f "/sys/class/net/${FEED_IFACE}/device/driver")")"

printf 'feed_iface=%s feed_ip=%s feed_pci=%s feed_numa=%s\n' \
  "$FEED_IFACE" "$FEED_IP" "$FEED_PCI" "$FEED_NUMA"
test "$FEED_IFACE" = enp40s0
test "$FEED_PCI" = 0000:28:00.0
test "$FEED_NUMA" = 0
test "$FEED_DRIVER" = ena
ip -o -4 addr show dev "$FEED_IFACE" |
  awk '{print $4}' | cut -d/ -f1 | grep -Fxq "$FEED_IP"
printf 'FEED_IFACE=%q\nFEED_IP=%q\nFEED_PCI=%q\nFEED_NUMA=%q\n' \
  "$FEED_IFACE" "$FEED_IP" "$FEED_PCI" "$FEED_NUMA" > "$STATE_FILE"
ethtool -i "$FEED_IFACE"
ip -br addr
```

Expected values are `0000:28:00.0`, driver `ena`, and NUMA node `0`. Stop if
the resolved PCI address is `0000:27:00.0` or belongs to the SSH interface.

## 4. Build, profile, derive capacity, and inspect the plan

Both EC2 roles use one build directory named `build` on their respective
hosts. The receiver configures its `build` with DPDK enabled; the external
sender later configures its own `build` with DPDK disabled. Do not force a
different generator when reusing an existing `build` directory:

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
REVISION_EVIDENCE="$RUN_EVIDENCE/revision"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
test -f "$STATE_FILE"
source "$STATE_FILE"

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=ON \
  -DASTRA_ENABLE_DPDK=ON \
  -DASTRA_ENABLE_IPO=OFF

cmake --build build --clean-first -j"$(nproc)"
ctest --test-dir build --output-on-failure
sha256sum build/md_engine build/benchmarks/astra_itch_trace_profile \
  build/benchmarks/astra_itch_book_replay_benchmark |
  tee "$REVISION_EVIDENCE/receiver-binaries.sha256"

BINARYFILE_RELATIVE_PATH="data/itch/unzipped/S061226-v50.txt"
TRACE="$PWD/$BINARYFILE_RELATIVE_PATH"
CORPUS_MANIFEST="$PWD/docs/trace-manifest-S061226-v50.txt"
PROFILE_OUTPUT="$PWD/data/itch/directory/S061226-v50.profile.txt"
PROFILER_BINARY="$PWD/data/itch/directory/tools/astra_itch_trace_profile-S061226-v2"
CAPACITY_EVIDENCE="$PWD/data/itch/directory/S061226-v50.capacity-evidence.txt"

mkdir -p "$(dirname "$PROFILER_BINARY")"
if test -e "$PROFILER_BINARY"; then
  cmp build/benchmarks/astra_itch_trace_profile "$PROFILER_BINARY"
else
  cp build/benchmarks/astra_itch_trace_profile "$PROFILER_BINARY"
fi
chmod 0555 "$PROFILER_BINARY"
sha256sum "$PROFILER_BINARY"

if ! "$PROFILER_BINARY" "$BINARYFILE_RELATIVE_PATH" \
    --allow-legacy-eof-after-sc | tee "$PROFILE_OUTPUT"; then
  echo "trace profiler failed" >&2
  exit 1
fi
grep -q 'certification semantic_clean=1' "$PROFILE_OUTPUT"

./scripts/derive_book_capacity_evidence.py \
  --profile-output "$PROFILE_OUTPUT" \
  --corpus-manifest "$CORPUS_MANIFEST" \
  --binaryfile "$TRACE" \
  --profiler-binary "$PROFILER_BINARY" \
  --profile-name nasdaq-itch-20260612-v1 \
  --minimum-direct-order-headroom 1 \
  --minimum-price-page-headroom 1 \
  --order-fallback-buckets 1 \
  --output "$CAPACITY_EVIDENCE"

capacity_value() {
  awk -F= -v key="$1" '$1 == key { print $2; found=1 }
    END { if (!found) exit 1 }' "$CAPACITY_EVIDENCE"
}

export ASTRA_BOOK_CAPACITY_PROFILE="$(capacity_value profile_name)"
export ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="$CAPACITY_EVIDENCE"
export ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256="$(
  sha256sum "$CAPACITY_EVIDENCE" | awk '{print $1}'
)"
export ASTRA_ORDER_DIRECT_SLOTS="$(capacity_value order_direct_slots)"
export ASTRA_ORDER_FALLBACK_BUCKETS="$(capacity_value order_fallback_buckets)"
export ASTRA_PRICE_PAGE_CAPACITY="$(capacity_value price_page_capacity)"
export ASTRA_PROFILED_MAX_ORDER_REF="$(capacity_value profiled_max_order_ref)"
export ASTRA_PROFILED_UNIQUE_PRICE_PAGES="$(
  capacity_value profiled_unique_price_pages
)"
export ASTRA_MIN_DIRECT_ORDER_HEADROOM="$(
  capacity_value minimum_direct_order_headroom
)"
export ASTRA_MIN_PRICE_PAGE_HEADROOM="$(
  capacity_value minimum_price_page_headroom
)"
export ASTRA_BOOK_PREFAULT=on

{
  printf 'BINARYFILE_RELATIVE_PATH=%q\n' "$BINARYFILE_RELATIVE_PATH"
  printf 'TRACE=%q\n' "$TRACE"
  printf 'CORPUS_MANIFEST=%q\n' "$CORPUS_MANIFEST"
  printf 'PROFILE_OUTPUT=%q\n' "$PROFILE_OUTPUT"
  printf 'PROFILER_BINARY=%q\n' "$PROFILER_BINARY"
  printf 'CAPACITY_EVIDENCE=%q\n' "$CAPACITY_EVIDENCE"
  printf 'ASTRA_BOOK_CAPACITY_PROFILE=%q\n' "$ASTRA_BOOK_CAPACITY_PROFILE"
  printf 'ASTRA_BOOK_CAPACITY_EVIDENCE_FILE=%q\n' "$ASTRA_BOOK_CAPACITY_EVIDENCE_FILE"
  printf 'ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=%q\n' "$ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256"
  printf 'ASTRA_ORDER_DIRECT_SLOTS=%q\n' "$ASTRA_ORDER_DIRECT_SLOTS"
  printf 'ASTRA_ORDER_FALLBACK_BUCKETS=%q\n' "$ASTRA_ORDER_FALLBACK_BUCKETS"
  printf 'ASTRA_PRICE_PAGE_CAPACITY=%q\n' "$ASTRA_PRICE_PAGE_CAPACITY"
  printf 'ASTRA_PROFILED_MAX_ORDER_REF=%q\n' "$ASTRA_PROFILED_MAX_ORDER_REF"
  printf 'ASTRA_PROFILED_UNIQUE_PRICE_PAGES=%q\n' "$ASTRA_PROFILED_UNIQUE_PRICE_PAGES"
  printf 'ASTRA_MIN_DIRECT_ORDER_HEADROOM=%q\n' "$ASTRA_MIN_DIRECT_ORDER_HEADROOM"
  printf 'ASTRA_MIN_PRICE_PAGE_HEADROOM=%q\n' "$ASTRA_MIN_PRICE_PAGE_HEADROOM"
} >> "$STATE_FILE"

CAPACITY_RUN_EVIDENCE="$RUN_EVIDENCE/capacity"
mkdir -p "$CAPACITY_RUN_EVIDENCE"
./build/md_engine --book-storage-plan-only |
  tee "$CAPACITY_RUN_EVIDENCE/book-storage-plan-S061226.txt"
cp --preserve=mode,timestamps "$CORPUS_MANIFEST" \
  "$CAPACITY_RUN_EVIDENCE/corpus-manifest.txt"
cp --preserve=mode,timestamps "$PROFILE_OUTPUT" \
  "$CAPACITY_RUN_EVIDENCE/trace-profile.txt"
cp --preserve=mode,timestamps "$PROFILER_BINARY" \
  "$CAPACITY_RUN_EVIDENCE/frozen-profiler"
cp --preserve=mode,timestamps "$CAPACITY_EVIDENCE" \
  "$CAPACITY_RUN_EVIDENCE/book-capacity-evidence.txt"
cmp "$CORPUS_MANIFEST" "$CAPACITY_RUN_EVIDENCE/corpus-manifest.txt"
cmp "$PROFILE_OUTPUT" "$CAPACITY_RUN_EVIDENCE/trace-profile.txt"
cmp "$PROFILER_BINARY" "$CAPACITY_RUN_EVIDENCE/frozen-profiler"
cmp "$CAPACITY_EVIDENCE" \
  "$CAPACITY_RUN_EVIDENCE/book-capacity-evidence.txt"
{
  sha256sum "$TRACE"
  sha256sum "$CAPACITY_RUN_EVIDENCE/corpus-manifest.txt"
  sha256sum "$CAPACITY_RUN_EVIDENCE/trace-profile.txt"
  sha256sum "$CAPACITY_RUN_EVIDENCE/frozen-profiler"
  sha256sum "$CAPACITY_RUN_EVIDENCE/book-capacity-evidence.txt"
  sha256sum "$CAPACITY_RUN_EVIDENCE/book-storage-plan-S061226.txt"
} > "$CAPACITY_RUN_EVIDENCE/artifacts.sha256"
test "$(stat -c %s "$TRACE")" = 41662444846
```

The profiler must exit zero and report `semantic_clean=1`; the derivation tool
also fails closed on malformed framing, timestamp/lifecycle/order anomalies,
nonzero final live orders/levels/pages, or a mismatched actual BinaryFILE hash
or size. It re-executes the supplied frozen profiler over the exact manifest
input label and requires byte-identical stdout before creating evidence. The
one-slot/page headroom and one fallback bucket are explicit minimums for this
one checksum-pinned replay only. They are not live-feed or multi-day sizing
policy.

Retain the profile output, frozen profiler and its SHA-256, corpus manifest,
capacity evidence and its SHA-256, and the storage plan under
`$RUN_EVIDENCE/capacity`. The reviewed local artifacts are
[`book-capacity-evidence-S061226-v50.txt`](book-capacity-evidence-S061226-v50.txt),
SHA-256
`55f5ba91d10c74ff28da877c3665a97dca69fe1c5a6572f64332ea56c30a5516`,
and
[`book-storage-plan-S061226-v50.txt`](book-storage-plan-S061226-v50.txt),
SHA-256
`9d79fb39de911edcaebdba4761b1b5736105b4d7ee0a367c5a548de54dd76674`.
They record 2,382,540,226 direct slots, one fallback bucket, 156,872 page
capacity, and `planned_storage_bytes=388036034560` (361.38671875 GiB).
The local evidence binds a macOS-built frozen-profiler SHA, so a Linux rebuild
must retain its freshly generated evidence hash and plan; the capacity values
and checksum-pinned profile observations must match or the run stops for
investigation. Confirm the selected NUMA node/cgroup has at least
405,215,903,744 bytes for plan plus 16 GiB, then separately leave the 4 GiB
DPDK reserve, OS memory, and operational headroom.

If the versioned profiler path already exists, do not overwrite it. Either
reuse it to produce the matching profile or choose a new versioned path, then
derive new evidence from that exact frozen executable.

If `build` already uses Unix Makefiles, omit `-G Ninja`. If it already uses
Ninja, omitting `-G` also preserves Ninja. Delete and recreate generated build
directories only when intentionally changing generators.

## 5. Reserve DPDK hugepages

The book arenas use anonymous transparent huge pages. DPDK separately
needs hugetlb pages for its mbuf pool:

```bash
set -euo pipefail

sudo mkdir -p /mnt/huge
mountpoint -q /mnt/huge ||
  sudo mount -t hugetlbfs -o pagesize=2M nodev /mnt/huge
sudo sysctl -w vm.nr_hugepages=2048
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo
test "$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo)" = 2048
test "$(awk '/HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 2048
test "$(awk '/HugePages_Free:/ {print $2}' /proc/meminfo)" -ge 2048
```

This reserves 4 GiB of 2 MiB pages. Do it before prefaulting the
arenas.

## 6. Remove Linux ownership and bind only the feed ENI

Locate the DPDK binding tool:

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
source "$STATE_FILE"

DPDK_DEVBIND="$(command -v dpdk-devbind.py ||
  printf '%s\n' /usr/share/dpdk/usertools/dpdk-devbind.py)"
test -x "$DPDK_DEVBIND"
printf 'DPDK_DEVBIND=%q\n' "$DPDK_DEVBIND" >> "$STATE_FILE"
sudo "$DPDK_DEVBIND" --status
```

Before changing any interface, fail closed against both the current SSH route
and the default route, and retain the kernel network state needed for recovery:

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
source "$STATE_FILE"

test -n "${SSH_CONNECTION:-}"
SSH_CLIENT_IP="${SSH_CONNECTION%% *}"
SSH_IFACE="$(
  ip -o route get "$SSH_CLIENT_IP" |
    awk '{for (i=1; i<=NF; ++i) if ($i=="dev") {print $(i+1); exit}}'
)"
DEFAULT_IFACE="$(
  ip -o route show default |
    awk '{for (i=1; i<=NF; ++i) if ($i=="dev") {print $(i+1); exit}}'
)"
test -n "$SSH_IFACE"
test -n "$DEFAULT_IFACE"

for PROTECTED_IFACE in "$SSH_IFACE" "$DEFAULT_IFACE"; do
  test "$FEED_IFACE" != "$PROTECTED_IFACE"
  PROTECTED_PCI="$(
    basename "$(readlink -f "/sys/class/net/${PROTECTED_IFACE}/device")"
  )"
  test "$FEED_PCI" != "$PROTECTED_PCI"
done

RECOVERY_DIR="$RUN_EVIDENCE/network-before-dpdk"
mkdir -p "$RECOVERY_DIR"
ip -br addr > "$RECOVERY_DIR/ip-address.txt"
ip route show table all > "$RECOVERY_DIR/ip-route-all.txt"
ip rule show > "$RECOVERY_DIR/ip-rule.txt"
ethtool -i "$FEED_IFACE" > "$RECOVERY_DIR/feed-ethtool.txt"
ip -o -4 addr show dev "$FEED_IFACE" |
  awk '{print $4}' | sort > "$RECOVERY_DIR/feed-ipv4.before.txt"
ip route show table 1001 > "$RECOVERY_DIR/table-1001.before.txt"
ip rule show |
  awk -v ip="$FEED_IP" '$0 ~ ("from " ip) && $0 ~ /lookup 1001/' \
  > "$RECOVERY_DIR/feed-rule-1001.before.txt"
test -s "$RECOVERY_DIR/table-1001.before.txt"
test "$(wc -l < "$RECOVERY_DIR/feed-rule-1001.before.txt")" -eq 1
printf 'ssh_client_ip=%s\nssh_iface=%s\ndefault_iface=%s\nfeed_iface=%s\nfeed_pci=%s\n' \
  "$SSH_CLIENT_IP" "$SSH_IFACE" "$DEFAULT_IFACE" "$FEED_IFACE" "$FEED_PCI" |
  tee "$RECOVERY_DIR/identity.txt"
printf 'SSH_CLIENT_IP=%q\nSSH_IFACE=%q\nDEFAULT_IFACE=%q\nRECOVERY_DIR=%q\n' \
  "$SSH_CLIENT_IP" "$SSH_IFACE" "$DEFAULT_IFACE" "$RECOVERY_DIR" \
  >> "$STATE_FILE"
```

Remove policy routing and addresses only from the secondary ENI:

```bash
set -euo pipefail

cd ~/astra-feed-engine
STATE_FILE="$PWD/../astra-run-evidence-S061226/run-state.env"
source "$STATE_FILE"

while IFS= read -r route; do
  [[ -z "$route" || "$route" == *" dev ${FEED_IFACE}"* ]] || {
    echo "routing table 1001 is not dedicated to the feed interface: $route" >&2
    exit 1
  }
done < <(ip route show table 1001)
while IFS= read -r rule; do
  if [[ "$rule" == *"lookup 1001"* &&
        "$rule" != *"from ${FEED_IP}"* ]]; then
    echo "routing table 1001 has a non-feed policy rule: $rule" >&2
    exit 1
  fi
done < <(ip rule show)

if test -s "$RECOVERY_DIR/feed-rule-1001.before.txt"; then
  sudo ip rule del from "$FEED_IP" table 1001
fi
sudo ip route flush table 1001
sudo ip addr flush dev "$FEED_IFACE"
sudo ip link set "$FEED_IFACE" down
```

Try normal VFIO/IOMMU isolation first:

```bash
set -euo pipefail

cd ~/astra-feed-engine
STATE_FILE="$PWD/../astra-run-evidence-S061226/run-state.env"
source "$STATE_FILE"

sudo modprobe vfio-pci
VFIO_BIND_STATE="$RECOVERY_DIR/vfio-bind.state"
VFIO_BIND_ERROR="$RECOVERY_DIR/vfio-bind-error.txt"
if sudo "$DPDK_DEVBIND" --bind=vfio-pci "$FEED_PCI" \
    2> "$VFIO_BIND_ERROR"; then
  printf 'normal_iommu\n' > "$VFIO_BIND_STATE"
else
  printf 'normal_failed\n' > "$VFIO_BIND_STATE"
  cat "$VFIO_BIND_ERROR" >&2
  echo "stop and diagnose; use no-IOMMU only for the exact documented failure" >&2
  exit 1
fi
```

The tested EC2 host reports:

```text
Error: IOMMU support is disabled, use --noiommu-mode for binding in noiommu mode
```

Only if the retained error contains the exact IOMMU-disabled diagnostic, and an
operator explicitly accepts the loss of DMA isolation on this dedicated
benchmark instance, start a new shell, set `ALLOW_UNSAFE_NOIOMMU=yes`, and run:

```bash
set -euo pipefail

cd ~/astra-feed-engine
STATE_FILE="$PWD/../astra-run-evidence-S061226/run-state.env"
source "$STATE_FILE"
test "${ALLOW_UNSAFE_NOIOMMU:-no}" = yes
test "$(cat "$RECOVERY_DIR/vfio-bind.state")" = normal_failed
grep -q 'IOMMU support is disabled' "$RECOVERY_DIR/vfio-bind-error.txt"

sudo modprobe vfio enable_unsafe_noiommu_mode=1
sudo modprobe vfio-pci
printf '1\n' |
  sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
printf 'unsafe_enabled\n' > "$RECOVERY_DIR/vfio-bind.state"
sudo "$DPDK_DEVBIND" --noiommu-mode --bind=vfio-pci "$FEED_PCI"
printf 'unsafe_noiommu\n' > "$RECOVERY_DIR/vfio-bind.state"
```

No-IOMMU VFIO removes DMA isolation. Never use it on a shared or production
host.

Confirm that only the secondary ENI moved:

```bash
set -euo pipefail

cd ~/astra-feed-engine
STATE_FILE="$PWD/../astra-run-evidence-S061226/run-state.env"
source "$STATE_FILE"

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

## 7. Start the DPDK engine

Use the capacity variables exported from the generated manifest in section 4.
Start the engine first:

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
source "$STATE_FILE"
sha256sum -c "$RUN_EVIDENCE/revision/receiver-binaries.sha256"
printf '%s  %s\n' "$ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256" \
  "$ASTRA_BOOK_CAPACITY_EVIDENCE_FILE" | sha256sum -c -

RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%S.%NZ)}"
printf 'run_id=%s\n' "$RUN_ID"
ENGINE_EVIDENCE="$RUN_EVIDENCE/engine/$RUN_ID"
if test -e "$ENGINE_EVIDENCE"; then
  echo "engine evidence already exists; choose a new RUN_ID" >&2
  exit 1
fi
mkdir -p "$ENGINE_EVIDENCE"
git rev-parse HEAD > "$ENGINE_EVIDENCE/git-head.txt"
cmp "$RUN_EVIDENCE/revision/git-head.txt" \
  "$ENGINE_EVIDENCE/git-head.txt"
git status --porcelain=v1 > "$ENGINE_EVIDENCE/git-status.txt"
test ! -s "$ENGINE_EVIDENCE/git-status.txt"
sha256sum scripts/run_engine_dpdk.sh \
  > "$ENGINE_EVIDENCE/launch-wrapper.sha256"
ENGINE_EAL_ARGS="--main-lcore 2 -l 2 --allow ${FEED_PCI} --huge-dir /mnt/huge --file-prefix astra"
{
  printf 'git_head=%s\n' "$(git rev-parse HEAD)"
  printf 'md_engine_sha256=%s\n' "$(sha256sum build/md_engine | awk '{print $1}')"
  printf 'ASTRA_BUILD_TYPE=Release\nASTRA_ENABLE_IPO=OFF\n'
  printf 'ASTRA_CPU=2\nASTRA_NUMA_NODE=%s\nASTRA_NUMA_MEM_POLICY=membind\n' "$FEED_NUMA"
  printf 'ASTRA_BOOK_CAPACITY_PROFILE=%s\n' "$ASTRA_BOOK_CAPACITY_PROFILE"
  printf 'ASTRA_BOOK_CAPACITY_EVIDENCE_FILE=%s\n' "$ASTRA_BOOK_CAPACITY_EVIDENCE_FILE"
  printf 'ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=%s\n' "$ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256"
  printf 'ASTRA_ORDER_DIRECT_SLOTS=%s\n' "$ASTRA_ORDER_DIRECT_SLOTS"
  printf 'ASTRA_ORDER_FALLBACK_BUCKETS=%s\n' "$ASTRA_ORDER_FALLBACK_BUCKETS"
  printf 'ASTRA_PRICE_PAGE_CAPACITY=%s\n' "$ASTRA_PRICE_PAGE_CAPACITY"
  printf 'ASTRA_PROFILED_MAX_ORDER_REF=%s\n' "$ASTRA_PROFILED_MAX_ORDER_REF"
  printf 'ASTRA_PROFILED_UNIQUE_PRICE_PAGES=%s\n' "$ASTRA_PROFILED_UNIQUE_PRICE_PAGES"
  printf 'ASTRA_MIN_DIRECT_ORDER_HEADROOM=%s\n' "$ASTRA_MIN_DIRECT_ORDER_HEADROOM"
  printf 'ASTRA_MIN_PRICE_PAGE_HEADROOM=%s\n' "$ASTRA_MIN_PRICE_PAGE_HEADROOM"
  printf 'ASTRA_BOOK_PREFAULT=on\nASTRA_LATENCY_METRICS=on\n'
  printf 'ASTRA_DPDK_PORT_ID=0\nASTRA_DPDK_QUEUE_ID=0\nASTRA_DPDK_BURST_SIZE=8\n'
  printf 'ASTRA_DPDK_RX_DESC=8192\nASTRA_DPDK_MEMPOOL_SIZE=65535\n'
  printf 'ASTRA_DPDK_MBUF_CACHE_SIZE=256\nASTRA_DPDK_PROMISCUOUS=off\n'
  printf 'ASTRA_DPDK_ALLMULTICAST=on\nASTRA_DPDK_SOCKET_ID=%s\n' "$FEED_NUMA"
  printf 'ASTRA_DPDK_LATENCY_MODE=packet\nASTRA_DPDK_FLOW_FILTER=off\n'
  printf 'ASTRA_DPDK_SKIP_BUILD=on\n'
  printf 'ASTRA_DPDK_EAL_ARGS=%s\n' "$ENGINE_EAL_ARGS"
} > "$ENGINE_EVIDENCE/launch.env"

sudo env \
  ASTRA_BUILD_TYPE=Release \
  ASTRA_ENABLE_IPO=OFF \
  ASTRA_CPU=2 \
  ASTRA_NUMA_NODE="$FEED_NUMA" \
  ASTRA_NUMA_MEM_POLICY=membind \
  ASTRA_BOOK_CAPACITY_PROFILE="$ASTRA_BOOK_CAPACITY_PROFILE" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="$ASTRA_BOOK_CAPACITY_EVIDENCE_FILE" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256="$ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256" \
  ASTRA_ORDER_DIRECT_SLOTS="$ASTRA_ORDER_DIRECT_SLOTS" \
  ASTRA_ORDER_FALLBACK_BUCKETS="$ASTRA_ORDER_FALLBACK_BUCKETS" \
  ASTRA_PRICE_PAGE_CAPACITY="$ASTRA_PRICE_PAGE_CAPACITY" \
  ASTRA_PROFILED_MAX_ORDER_REF="$ASTRA_PROFILED_MAX_ORDER_REF" \
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES="$ASTRA_PROFILED_UNIQUE_PRICE_PAGES" \
  ASTRA_MIN_DIRECT_ORDER_HEADROOM="$ASTRA_MIN_DIRECT_ORDER_HEADROOM" \
  ASTRA_MIN_PRICE_PAGE_HEADROOM="$ASTRA_MIN_PRICE_PAGE_HEADROOM" \
  ASTRA_BOOK_PREFAULT=on \
  ASTRA_LATENCY_METRICS=on \
  ASTRA_DPDK_PORT_ID=0 \
  ASTRA_DPDK_QUEUE_ID=0 \
  ASTRA_DPDK_BURST_SIZE=8 \
  ASTRA_DPDK_RX_DESC=8192 \
  ASTRA_DPDK_MEMPOOL_SIZE=65535 \
  ASTRA_DPDK_MBUF_CACHE_SIZE=256 \
  ASTRA_DPDK_PROMISCUOUS=off \
  ASTRA_DPDK_ALLMULTICAST=on \
  ASTRA_DPDK_SOCKET_ID="$FEED_NUMA" \
  ASTRA_DPDK_LATENCY_MODE=packet \
  ASTRA_DPDK_FLOW_FILTER=off \
  ASTRA_DPDK_SKIP_BUILD=on \
  ASTRA_DPDK_EAL_ARGS="$ENGINE_EAL_ARGS" \
  ./scripts/run_engine_dpdk.sh \
    "$FEED_IP" 9000 "$FEED_IP" 9001 2>&1 |
  tee "$ENGINE_EVIDENCE/engine.log"
```

`ASTRA_DPDK_FLOW_FILTER=off` is intentional for the tested AWS ENA PMD. The
userspace parser still validates Ethernet/VLAN, IPv4, fragmentation,
UDP lengths, destination address, and destination port before accepting a
payload. It does not currently enforce IPv4 or UDP checksums, so a clean replay
is not checksum-path production certification. Every DPDK setting above is
pinned explicitly because the engine currently reports only part of the
effective receiver configuration. Retain the launch environment with the log.
Start with burst size 8 for latency; test larger bursts separately for
throughput. In `packet` mode, the
latency sample starts after
`rte_eth_rx_burst` returns and includes frame parsing plus ITCH decode/book
mutation. It is a processing-hot-path measurement, not kernel-versus-
DPDK transport timing.

Successful initialization on the tested host includes:

```text
EAL: Detected CPU lcores: 32
EAL: Detected NUMA nodes: 1
EAL: Selected IOVA mode 'PA'
EAL: Using IOMMU type ... <normal IOMMU, or explicitly accepted No-IOMMU>
cpu_affinity status=applied cpu=2 phase=post_dpdk_eal
book_capacity_profile name=nasdaq-itch-20260612-v1 ...
book_storage_plan ... planned_storage_bytes=388036034560
```

After `book_storage_plan`, the process maps and prefaults the exact derived
capacity on the selected NUMA node. A long period without new output can be
expected for a large plan. Do not interrupt the engine or start the sender
until both `book_storage ...` and `Engine started ...` appear.

## 8. Start the external sender

Run this on the sender EC2 instance, not on the DPDK receiver host. Security
groups and network ACLs must allow UDP ports 9000 and 9001 to `172.31.32.18`.
The synchronized feeder reads one source stream and sends the exact same
MoldUDP64 packet to line A and line B before advancing. Copy the full receiver
Git hash from `$RUN_EVIDENCE/revision/git-head.txt` and export it as
`EXPECTED_GIT_HEAD` on the sender. Also export the receiver's printed `RUN_ID`
so the two logs share one attempt identity:

Generated data packets end at System Hours `S` and Market Hours `Q`, so
`ASTRA_SS_PAUSE_SECONDS` and premarket pacing take effect before the next ITCH
record rather than after the remainder of a larger synthetic packet.
The ordinary `pkt/s` limiter is a hard upper rate and does not catch up with
bursts after delays. This shared-IP A/B sender also rejects equal destination
ports. Timestamp mode uses one-message packets between `S` and `Q`, then
restores the requested message ceiling. Any physical send failure stops replay
without an EOS announcement or logical commit for the failed packet.

```bash
set -euo pipefail

: "${EXPECTED_GIT_HEAD:?export the receiver git HEAD before continuing}"
: "${RUN_ID:?export the receiver RUN_ID before continuing}"

sudo apt-get update
sudo apt-get install -y build-essential cmake git numactl python3

cd ~/astra-feed-engine
test "$(git rev-parse HEAD)" = "$EXPECTED_GIT_HEAD"
test -z "$(git status --porcelain=v1)"
SENDER_EVIDENCE="$PWD/../astra-run-evidence-S061226/sender/$RUN_ID"
if test -e "$SENDER_EVIDENCE"; then
  echo "sender evidence already exists; choose a new shared RUN_ID" >&2
  exit 1
fi
mkdir -p "$SENDER_EVIDENCE"
git rev-parse HEAD > "$SENDER_EVIDENCE/git-head.txt"
git status --porcelain=v1 > "$SENDER_EVIDENCE/git-status.txt"
test ! -s "$SENDER_EVIDENCE/git-status.txt"
cmake --version > "$SENDER_EVIDENCE/cmake-version.txt"
"${CXX:-c++}" --version > "$SENDER_EVIDENCE/compiler-version.txt"

TRACE="$PWD/data/itch/unzipped/S061226-v50.txt"
test "$(stat -c %s "$TRACE")" = 41662444846
test "$(sha256sum "$TRACE" | awk '{print $1}')" = \
  8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=ON \
  -DASTRA_ENABLE_DPDK=OFF \
  -DASTRA_ENABLE_IPO=OFF

cmake --build build --clean-first -j"$(nproc)"
ctest --test-dir build --output-on-failure
sha256sum build/itch_moldudp_sender |
  tee "$SENDER_EVIDENCE/sender-binary.sha256"
sha256sum scripts/run_sender.sh > "$SENDER_EVIDENCE/launch-wrapper.sha256"
sha256sum "$TRACE" | tee "$SENDER_EVIDENCE/trace.sha256"

{
  printf 'git_head=%s\n' "$EXPECTED_GIT_HEAD"
  printf 'sender_sha256=%s\n' "$(sha256sum build/itch_moldudp_sender | awk '{print $1}')"
  printf 'trace_sha256=%s\n' "$(sha256sum "$TRACE" | awk '{print $1}')"
  printf 'ASTRA_BUILD_TYPE=Release\nASTRA_ENABLE_IPO=OFF\nASTRA_SENDER_SKIP_BUILD=on\n'
  printf 'ASTRA_CPU_A=3\nASTRA_CPU_B=4\nASTRA_LINE_B_DELAY_NS=1000\n'
  printf 'ASTRA_STARTUP_HEARTBEAT_COUNT=0\nASTRA_HEARTBEAT_INTERVAL_MS=1000\n'
  printf 'ASTRA_EOS_PACKET_COUNT=10\nASTRA_EOS_INTERVAL_MS=100\n'
  printf 'ASTRA_PREMARKET_REPLAY_MODE=off\nASTRA_SS_PAUSE_SECONDS=120\n'
  printf 'ASTRA_BINARYFILE_COMPLETION=legacy-sc-eof\n'
  printf 'destination=172.31.32.18:9000,9001\nmessages_per_packet=20\n'
  printf 'session=%s\npackets_per_second=50000\n' "ASTRA     "
} > "$SENDER_EVIDENCE/launch.env"

ASTRA_BUILD_TYPE=Release \
ASTRA_ENABLE_IPO=OFF \
ASTRA_SENDER_SKIP_BUILD=on \
ASTRA_CPU_A=3 \
ASTRA_CPU_B=4 \
ASTRA_LINE_B_DELAY_NS=1000 \
ASTRA_STARTUP_HEARTBEAT_COUNT=0 \
ASTRA_HEARTBEAT_INTERVAL_MS=1000 \
ASTRA_EOS_PACKET_COUNT=10 \
ASTRA_EOS_INTERVAL_MS=100 \
ASTRA_PREMARKET_REPLAY_MODE=off \
ASTRA_SS_PAUSE_SECONDS=120 \
ASTRA_BINARYFILE_COMPLETION=legacy-sc-eof \
./scripts/run_sender.sh \
  "$TRACE" \
  172.31.32.18 \
  9000 \
  9001 \
  20 \
  "ASTRA     " \
  50000 2>&1 |
  tee "$SENDER_EVIDENCE/sender.log"
```

The pinned SHA-256 above identifies an official 2026 BinaryFILE that has been
independently verified to end physically after System Event `C` without a
zero-length terminator, so this command opts into that compatibility policy
explicitly. `ASTRA_BINARYFILE_COMPLETION=strict` remains the default for all
other inputs; never select `legacy-sc-eof` from a filename, age, or provider
alone. The sender does not hash-bind compatibility mode itself; the explicit
size and SHA-256 checks above are therefore required before launch.

Start the engine first and wait for its ready marker before starting the
sender. The explicit zero startup-heartbeat count makes sequence `1` the first
data packet presented to the measured path, even if the launching environment
contains old settings. Count-zero heartbeats are emitted on both lines at each
1,000-ms idle deadline that precedes the next data send during pacing or an
`SS` pause.
After clean SC-plus-EOF completion, the sender skips ordinary pacing and idle
heartbeats and immediately begins ten MoldUDP64 end-of-session packets on each
line, with at least 100 ms between successful sends.

The `20` messages-per-packet setting is a ceiling rather than a promise. The
sender flushes a packet early when the next record would make the UDP payload
exceed 1,472 bytes, which fits a standard 1,500-byte IPv4 MTU without
fragmentation. The kernel, DPDK, replay, and gap-storage receive capacity
remains 2,048 bytes.

The lifecycle admits Stock Directory `R` before End of System Hours, including
after Start of System Hours. It does not admit a late directory message after
System Event `E`: from `E` to terminal `C`, partial Order Cancel `X` and Order
Delete `D` are valid against existing orders, while Broken Trade `B` is
accepted as having no current-L2-book effect. Match-number history is not
retained, so `B` is not cross-validated.
The current specification names `B` and `D`; the official checksum-pinned
2026-06-12 archive additionally contains 12 valid `X` messages interleaved
with `D` in the post-`E` teardown. System Event `C` is accepted only after
that tail leaves zero live orders.

## 9. Run correctness observation separately from latency

On the receiver host, use zero book-message warmup for the whole-day
correctness/digest process. This prevents a performance warmup from being
mistaken for an excluded correctness prefix:

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
source "$STATE_FILE"
CORRECTNESS_RUN_ID="$(
  date -u +%Y%m%dT%H%M%S.%NZ
)"
CORRECTNESS_EVIDENCE="$RUN_EVIDENCE/correctness/$CORRECTNESS_RUN_ID"
if test -e "$CORRECTNESS_EVIDENCE"; then
  echo "correctness evidence ID collision; rerun with a fresh timestamp" >&2
  exit 1
fi
mkdir -p "$CORRECTNESS_EVIDENCE"
git rev-parse HEAD > "$CORRECTNESS_EVIDENCE/git-head.txt"
cmp "$RUN_EVIDENCE/revision/git-head.txt" \
  "$CORRECTNESS_EVIDENCE/git-head.txt"
git status --porcelain=v1 > "$CORRECTNESS_EVIDENCE/git-status.txt"
test ! -s "$CORRECTNESS_EVIDENCE/git-status.txt"
sha256sum -c "$RUN_EVIDENCE/revision/receiver-binaries.sha256"
test "$(stat -c %s "$TRACE")" = 41662444846
test "$(sha256sum "$TRACE" | awk '{print $1}')" = \
  8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238
printf '%s  %s\n' "$ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256" \
  "$ASTRA_BOOK_CAPACITY_EVIDENCE_FILE" | sha256sum -c -

PROFILE_RECORDS="$(
  awk '$1 == "itch_trace_profile" {
    for (i = 2; i <= NF; ++i) {
      if ($i ~ /^records=/) {
        sub(/^records=/, "", $i)
        print $i
        exit
      }
    }
  }' "$PROFILE_OUTPUT"
)"
test -n "$PROFILE_RECORDS"

REPLAY_COMMAND=(
  numactl --physcpubind=2 --membind="$FEED_NUMA"
  build/benchmarks/astra_itch_book_replay_benchmark
  "$TRACE"
  --allow-legacy-eof-after-sc
  --prefault
  "--direct-order-slots=$ASTRA_ORDER_DIRECT_SLOTS"
  "--fallback-buckets=$ASTRA_ORDER_FALLBACK_BUCKETS"
  "--price-page-capacity=$ASTRA_PRICE_PAGE_CAPACITY"
  "--capacity-profile-name=$ASTRA_BOOK_CAPACITY_PROFILE"
  "--capacity-evidence-file=$ASTRA_BOOK_CAPACITY_EVIDENCE_FILE"
  "--capacity-evidence-sha256=$ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256"
  --sample-every=1024
  --warmup-book-messages=0
  --min-samples=1000
  "--expect-records=$PROFILE_RECORDS"
  --expect-bytes=41662444846
)
"${REPLAY_COMMAND[@]}" --mutation-digest 2>&1 |
  tee "$CORRECTNESS_EVIDENCE/digest-discovery.log"
```

In that same shell, copy the two printed values and run an independent
verification process into a different retained log:

```bash
set -euo pipefail

: "${EXPECTED_MUTATION_DIGEST:?set the discovered physical digest}"
: "${EXPECTED_SEMANTIC_MUTATION_DIGEST:?set the discovered semantic digest}"
"${REPLAY_COMMAND[@]}" \
  "--expect-mutation-digest=$EXPECTED_MUTATION_DIGEST" \
  "--expect-semantic-mutation-digest=$EXPECTED_SEMANTIC_MUTATION_DIGEST" \
  2>&1 | tee "$CORRECTNESS_EVIDENCE/digest-verified.log"
```

Do not attach latency thresholds to a mutation-digest run because the extra
state reads change cache behavior. Every new attempt must use a new evidence
directory; the blocks fail instead of overwriting retained results.

Use `--warmup-book-messages=1000000` only in a separate steady-state latency
process, without mutation digests, with prefaulting, an isolated CPU/local
NUMA node, exact record/byte gates, and pre-approved absolute latency ceilings.
That warm latency process is performance evidence, not the whole-day
correctness process.

## 10. Validate the result

A clean full-trace run has:

```text
sender_stats completion=complete
line_a_send_failures=0
line_b_send_failures=0
startup_heartbeats_sent=0
first_seq=1
eos_packets_sent=10
eos_packets_expected=10
binaryfile_completion=legacy_sc_eof
end_of_session_sent=true
sender_stats ... next_seq=1304894065
Engine stopped  symbols=12809
engine_stats channel_next_seq=1304894065
channel_status_name=Good
channel_phase_name=EndOfMessages
conflicting_buffered_redundant_packets=0
final_live_orders=0
registered_symbols=12809
materialized_books=12809
prepared_books=12809
registered_books_missing=0
unregistered_books_present=0
descriptor_price_state_mismatches=0
descriptor_identity_mismatches=0
committed_price_pages=156871
price_page_capacity=156872
price_page_capacity_failures=0
end_of_stream_accepted=true
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
the run. Engine exit also fails unless EOS was accepted after terminal System
Event `C`, phase is End of Messages, the gap buffer is empty, channel health is
Good, and no live orders remain. The semantic profile must finish with zero
live orders, zero active levels, and zero active price pages. The
production-book correctness replay must report End of Messages,
`final_live_orders=0`, all 12,809 registered symbols materialized and prepared,
no unregistered or descriptor/price-state mismatch, 156,871 committed
monotonic price pages, and zero page-capacity failures. Every materialized
descriptor must also retain the locate identity of its fixed slot. Engine exit
now enforces the generic per-directory book audit and zero capacity failures;
the runbook additionally checks the checksum-pinned corpus's exact counts. The
committed-page count remains nonzero after every active level becomes empty.
Do not treat the first process as final evidence; repeat with identical host
state and retain every result.

Clean completion here is checksum-pinned replay certification. It does not
remove the live-readiness gaps listed at the top of this runbook. In
particular, redundant replay does not create a re-request service, a receiver
watchdog, or independent A/B health state.

## 11. Return the ENI to Linux

```bash
set -euo pipefail

cd ~/astra-feed-engine
RUN_EVIDENCE="$PWD/../astra-run-evidence-S061226"
STATE_FILE="$RUN_EVIDENCE/run-state.env"
source "$STATE_FILE"

sudo modprobe ena
sudo "$DPDK_DEVBIND" --bind=ena "$FEED_PCI"
sudo "$DPDK_DEVBIND" --status

if test -r /sys/module/vfio/parameters/enable_unsafe_noiommu_mode &&
    grep -Eq '^(1|Y)$' \
      /sys/module/vfio/parameters/enable_unsafe_noiommu_mode; then
  printf '0\n' |
    sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
fi

./scripts/setup_secondary_eni.sh \
  --iface "$FEED_IFACE" \
  --table 1001 \
  --priority 1001
test "$(basename "$(readlink -f "/sys/class/net/${FEED_IFACE}/device")")" = \
  "$FEED_PCI"
test "$(basename "$(readlink -f "/sys/class/net/${FEED_IFACE}/device/driver")")" = \
  ena
ip -br addr show dev "$FEED_IFACE"
ip route get "$SSH_CLIENT_IP"

ip -o -4 addr show dev "$FEED_IFACE" |
  awk '{print $4}' | sort > "$RECOVERY_DIR/feed-ipv4.after.txt"
ip route show table 1001 > "$RECOVERY_DIR/table-1001.after.txt"
ip rule show |
  awk -v ip="$FEED_IP" '$0 ~ ("from " ip) && $0 ~ /lookup 1001/' \
  > "$RECOVERY_DIR/feed-rule-1001.after.txt"
diff -u "$RECOVERY_DIR/feed-ipv4.before.txt" \
  "$RECOVERY_DIR/feed-ipv4.after.txt"
diff -u "$RECOVERY_DIR/table-1001.before.txt" \
  "$RECOVERY_DIR/table-1001.after.txt"
diff -u "$RECOVERY_DIR/feed-rule-1001.before.txt" \
  "$RECOVERY_DIR/feed-rule-1001.after.txt"
test "$(
  ip -o route get "$SSH_CLIENT_IP" |
    awk '{for (i=1; i<=NF; ++i) if ($i=="dev") {print $(i+1); exit}}'
)" = "$SSH_IFACE"
```

The diffs must be empty. The block disables unsafe no-IOMMU mode when this
run enabled it, but deliberately leaves the 4 GiB hugepage reservation and
`/mnt/huge` mount in place in case retained logs require another replay. On a
dedicated host, after all replays finish and no other DPDK process uses them,
release those resources explicitly:

```bash
set -euo pipefail

test "${RELEASE_DPDK_HUGEPAGES:-no}" = yes
sudo sysctl -w vm.nr_hugepages=0
sudo umount /mnt/huge
```

## Same-host UDP smoke test

For a quick one-instance lifecycle smoke test, do not bind the ENI to DPDK.
Use `127.0.0.1:9000/9001` with the Release UDP binary. This validates parsing
and book behavior but is not DPDK or deterministic EC2 latency evidence.
Run scalar mode with `ASTRA_UDP_DROP_METRICS=on` and require
`drop_metrics=on`, `line_a_kernel_drops=0`, and
`line_b_kernel_drops=0`. Linux batch mode enables overflow telemetry
unconditionally and both kernel-drop counters must be zero.

## Troubleshooting

- `Package 'libdpdk' not found`: install `libdpdk-dev` and verify
  `pkg-config --modversion libdpdk`.
- `generator Ninja does not match Unix Makefiles`: omit `-G Ninja` when
  reusing `build`, or recreate `build` before intentionally changing
  generators.
- `Gap meet channel_expected_seq=1 packet_first_seq=...`: the first sequenced
  datagram was lost or the receiver was not ready. Stop both processes, retain
  both logs, verify the engine ready marker and network path, and restart from
  sequence 1. The decoder never infers a new starting sequence from an
  arbitrary first datagram.
- `IOMMU support is disabled`: on a dedicated benchmark host only, use the
  documented unsafe VFIO no-IOMMU procedure and bind with `--noiommu-mode`.
- `no DPDK Ethernet ports are available`: check the VFIO binding and that the
  EAL allowlist exactly matches `$FEED_PCI`.
- Engine receives nothing: verify the sender targets `172.31.32.18`, both UDP
  ports are allowed, and the secondary ENI is the only allowlisted device.
- `Cannot allocate memory`: recheck free node/cgroup memory, DPDK hugepages,
  swap/THP policy, and the checksum-bound storage plan before starting the
  process.
- CPU affinity failure: CPU 2 must be online and permitted by the service or
  shell cgroup. Leave sibling CPU 18 idle.
