#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dpdk_ec2_config.sh
source "${SCRIPT_DIR}/dpdk_ec2_config.sh"

usage() {
  cat <<'USAGE'
Usage: ./scripts/dpdk/dpdk_04_run.sh

Load the checked-in S061226 capacity configuration and start md_engine on the
DPDK-owned feed ENI. Latency metrics default to on and can be disabled with
ASTRA_LATENCY_METRICS=off. This script does not run a sender or trace.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

driver_for_pci() {
  local driver_path="/sys/bus/pci/devices/$1/driver"
  [[ -L "${driver_path}" ]] || return 1
  basename "$(readlink -f "${driver_path}")"
}

if [[ "$(uname -s)" != Linux ]]; then
  echo "The DPDK engine is Linux-only." >&2
  exit 1
fi
for command_name in awk basename findmnt grep lscpu mountpoint numactl \
                    readlink sha256sum; do
  require_command "${command_name}"
done
if [[ "${EUID}" -ne 0 ]]; then
  require_command sudo
fi

if [[ ! -x "${ASTRA_REPO_ROOT}/build/md_engine" ]]; then
  echo "Missing ${ASTRA_REPO_ROOT}/build/md_engine; run scripts/dpdk/dpdk_02_build.sh first." >&2
  exit 1
fi
if [[ ! -f "${ASTRA_REPO_ROOT}/build/CMakeCache.txt" ]] ||
    ! grep -Fxq 'ASTRA_ENABLE_DPDK:BOOL=ON' \
      "${ASTRA_REPO_ROOT}/build/CMakeCache.txt"; then
  echo "build/md_engine was not configured with ASTRA_ENABLE_DPDK=ON." >&2
  echo "Run scripts/dpdk/dpdk_02_build.sh before starting the engine." >&2
  exit 1
fi
astra_load_book_capacity
astra_require_capacity_admission check-runtime-memory
if [[ ! -r "${ASTRA_DPDK_STATE_FILE}" ]]; then
  echo "Missing DPDK bind state: ${ASTRA_DPDK_STATE_FILE}" >&2
  echo "Run scripts/dpdk/dpdk_03_bind.sh before starting the engine." >&2
  exit 1
fi
# shellcheck disable=SC1090
source "${ASTRA_DPDK_STATE_FILE}"
if [[ "${ASTRA_SAVED_FEED_IFACE:-}" != "${ASTRA_FEED_IFACE}" ||
      "${ASTRA_SAVED_FEED_IP:-}" != "${ASTRA_FEED_IP}" ||
      "${ASTRA_SAVED_FEED_PCI:-}" != "${ASTRA_FEED_PCI}" ||
      "${ASTRA_SAVED_FEED_NUMA:-}" != "${ASTRA_FEED_NUMA}" ||
      "${ASTRA_BIND_COMPLETE:-0}" != 1 ]]; then
  echo "Current feed configuration does not match dpdk_03_bind.sh state." >&2
  echo "Run scripts/dpdk/dpdk_05_restore.sh, then repeat step 3." >&2
  exit 1
fi

if [[ "$(driver_for_pci "${ASTRA_FEED_PCI}" || true)" != vfio-pci ]]; then
  echo "${ASTRA_FEED_PCI} is not bound to vfio-pci; run scripts/dpdk/dpdk_03_bind.sh first." >&2
  exit 1
fi
if ! mountpoint -q "${ASTRA_DPDK_HUGE_DIR}"; then
  echo "Hugepage filesystem is not mounted at ${ASTRA_DPDK_HUGE_DIR}." >&2
  exit 1
fi
HUGE_FSTYPE="$(
  findmnt -n -o FSTYPE --target "${ASTRA_DPDK_HUGE_DIR}"
)"
HUGE_OPTIONS="$(
  findmnt -n -o OPTIONS --target "${ASTRA_DPDK_HUGE_DIR}"
)"
if [[ "${HUGE_FSTYPE}" != hugetlbfs ||
      ",${HUGE_OPTIONS}," != *,pagesize=2M,* ]]; then
  echo "${ASTRA_DPDK_HUGE_DIR} is not a 2 MiB hugetlbfs mount." >&2
  exit 1
fi
if [[ ! "${ASTRA_ENGINE_CPU}" =~ ^[0-9]+$ ]]; then
  echo "ASTRA_ENGINE_CPU must be a non-negative CPU ID." >&2
  exit 2
fi
if [[ ! "${ASTRA_FEED_NUMA}" =~ ^[0-9]+$ ]]; then
  echo "ASTRA_FEED_NUMA must be a non-negative NUMA node." >&2
  exit 2
fi
if [[ ! "${ASTRA_DPDK_HUGE_PAGES}" =~ ^[1-9][0-9]*$ ]]; then
  echo "ASTRA_DPDK_HUGE_PAGES must be a positive integer." >&2
  exit 2
fi
if [[ ! "${ASTRA_DPDK_FILE_PREFIX}" =~ ^[[:alnum:]_.-]+$ ]]; then
  echo "ASTRA_DPDK_FILE_PREFIX contains unsupported characters." >&2
  exit 2
fi

HUGEPAGE_FREE_FILE="/sys/devices/system/node/node${ASTRA_FEED_NUMA}/hugepages/hugepages-2048kB/free_hugepages"
if [[ -r "${HUGEPAGE_FREE_FILE}" ]]; then
  FREE_HUGEPAGES="$(<"${HUGEPAGE_FREE_FILE}")"
  if (( FREE_HUGEPAGES < ASTRA_DPDK_HUGE_PAGES )); then
    echo "Only ${FREE_HUGEPAGES} free 2 MiB hugepages are available on NUMA node ${ASTRA_FEED_NUMA}." >&2
    echo "Stop other DPDK processes or rerun scripts/dpdk/dpdk_03_bind.sh." >&2
    exit 1
  fi
fi

CPU_RECORD="$(
  lscpu -p=CPU,NODE,ONLINE |
    awk -F, -v cpu="${ASTRA_ENGINE_CPU}" '
      $1 !~ /^#/ && $1 == cpu {print $2 "," $3; exit}
    '
)"
if [[ -z "${CPU_RECORD}" ]]; then
  echo "CPU ${ASTRA_ENGINE_CPU} does not exist." >&2
  exit 1
fi
IFS=, read -r CPU_NODE CPU_ONLINE <<<"${CPU_RECORD}"
if [[ "${CPU_ONLINE}" != Y && "${CPU_ONLINE}" != yes &&
      "${CPU_ONLINE}" != 1 ]]; then
  echo "CPU ${ASTRA_ENGINE_CPU} is not online." >&2
  exit 1
fi
if [[ "${CPU_NODE}" != "${ASTRA_FEED_NUMA}" ]]; then
  echo "CPU ${ASTRA_ENGINE_CPU} is on NUMA node ${CPU_NODE}, not feed node ${ASTRA_FEED_NUMA}." >&2
  exit 1
fi

EAL_ARGS="--main-lcore ${ASTRA_ENGINE_CPU} -l ${ASTRA_ENGINE_CPU} --allow ${ASTRA_FEED_PCI} --huge-dir ${ASTRA_DPDK_HUGE_DIR} --file-prefix ${ASTRA_DPDK_FILE_PREFIX} --huge-unlink=always"

RUNNER=(env)
if [[ "${EUID}" -ne 0 ]]; then
  RUNNER=(sudo env)
fi

echo "Starting DPDK engine on ${ASTRA_FEED_IP}:9000/9001..."
exec "${RUNNER[@]}" \
  ASTRA_CPU="${ASTRA_ENGINE_CPU}" \
  ASTRA_NUMA_NODE="${ASTRA_FEED_NUMA}" \
  ASTRA_NUMA_MEM_POLICY=membind \
  ASTRA_BOOK_CAPACITY_PROFILE="${ASTRA_BOOK_CAPACITY_PROFILE}" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="${ASTRA_BOOK_CAPACITY_FILE}" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256="${ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256}" \
  ASTRA_ORDER_DIRECT_SLOTS="${ASTRA_ORDER_DIRECT_SLOTS}" \
  ASTRA_ORDER_FALLBACK_BUCKETS="${ASTRA_ORDER_FALLBACK_BUCKETS}" \
  ASTRA_PRICE_PAGE_CAPACITY="${ASTRA_PRICE_PAGE_CAPACITY}" \
  ASTRA_PROFILED_MAX_ORDER_REF="${ASTRA_PROFILED_MAX_ORDER_REF}" \
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES="${ASTRA_PROFILED_UNIQUE_PRICE_PAGES}" \
  ASTRA_MIN_DIRECT_ORDER_HEADROOM="${ASTRA_MIN_DIRECT_ORDER_HEADROOM}" \
  ASTRA_MIN_PRICE_PAGE_HEADROOM="${ASTRA_MIN_PRICE_PAGE_HEADROOM}" \
  ASTRA_BOOK_PREFAULT="${ASTRA_BOOK_PREFAULT}" \
  ASTRA_LATENCY_METRICS="${ASTRA_LATENCY_METRICS:-on}" \
  ASTRA_DPDK_PORT_ID=0 \
  ASTRA_DPDK_QUEUE_ID=0 \
  ASTRA_DPDK_BURST_SIZE=32 \
  ASTRA_DPDK_RX_DESC=8192 \
  ASTRA_DPDK_MEMPOOL_SIZE=65535 \
  ASTRA_DPDK_MBUF_CACHE_SIZE=256 \
  ASTRA_DPDK_PROMISCUOUS=off \
  ASTRA_DPDK_ALLMULTICAST=on \
  ASTRA_DPDK_SOCKET_ID="${ASTRA_FEED_NUMA}" \
  ASTRA_DPDK_FLOW_FILTER=off \
  ASTRA_DPDK_SKIP_BUILD=on \
  ASTRA_DPDK_EAL_ARGS="${EAL_ARGS}" \
  "${ASTRA_REPO_ROOT}/scripts/run_engine_dpdk.sh" \
  "${ASTRA_FEED_IP}" 9000 "${ASTRA_FEED_IP}" 9001
