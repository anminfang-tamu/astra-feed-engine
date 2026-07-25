#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/md_engine"
BUILD_TYPE="${ASTRA_BUILD_TYPE:-Release}"
SKIP_BUILD="${ASTRA_DPDK_SKIP_BUILD:-off}"

NUMA_NODE="${ASTRA_NUMA_NODE:-}"
NUMA_MEM_POLICY="${ASTRA_NUMA_MEM_POLICY:-membind}"

usage() {
  cat <<'USAGE'
Usage:
  ./scripts/run_engine_dpdk.sh
  ./scripts/run_engine_dpdk.sh --help
  ./scripts/run_engine_dpdk.sh <ip>
  ./scripts/run_engine_dpdk.sh <ip> <port> [channel_id]
  ./scripts/run_engine_dpdk.sh <ip_a> <port_a> <ip_b> <port_b> [channel_id]

Default: DPDK A/B receiver on 0.0.0.0:9000 and 0.0.0.0:9001.

Required deployment environment:
  ASTRA_BOOK_CAPACITY_PROFILE=<approved-profile>
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE=<canonical-manifest>
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=<manifest-sha256>
  ASTRA_ORDER_DIRECT_SLOTS=<manifest-value>
  ASTRA_ORDER_FALLBACK_BUCKETS=<manifest-value>
  ASTRA_PRICE_PAGE_CAPACITY=<manifest-value>
  ASTRA_PROFILED_MAX_ORDER_REF=<manifest-value>
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES=<manifest-value>
  ASTRA_MIN_DIRECT_ORDER_HEADROOM=<manifest-value>
  ASTRA_MIN_PRICE_PAGE_HEADROOM=<manifest-value>

Useful environment:
  ASTRA_BOOK_PREFAULT=on
  ASTRA_DPDK_PORT_ID=0
  ASTRA_DPDK_QUEUE_ID=0
  ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2"
  ASTRA_DPDK_BURST_SIZE=32  # must be divisible by 8
  ASTRA_DPDK_LATENCY_MODE=packet  # packet (default) or burst
  ASTRA_DPDK_FLOW_FILTER=off  # AWS ENA-safe default; set on only if supported
  ASTRA_DPDK_RX_DESC=4096
  ASTRA_DPDK_MEMPOOL_SIZE=65535
  ASTRA_DPDK_MBUF_CACHE_SIZE=256
  ASTRA_DPDK_PROMISCUOUS=off
  ASTRA_DPDK_ALLMULTICAST=on
  ASTRA_DPDK_SOCKET_ID=<NUMA node>
  ASTRA_DPDK_SKIP_BUILD=on  # use an already-built binary
  ASTRA_LATENCY_METRICS=on
  ASTRA_BUILD_TYPE=Release  # default
  ASTRA_ENABLE_IPO=ON       # optional compiler-supported LTO/IPO
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

case "$#" in
  0)
    ENGINE_ARGS=(0.0.0.0 9000 0.0.0.0 9001)
    ;;
  1)
    ENGINE_ARGS=("$1" 9000 "$1" 9001)
    ;;
  2|3|4|5)
    ENGINE_ARGS=("$@")
    ;;
  *)
    echo "Invalid arguments." >&2
    usage >&2
    exit 2
    ;;
esac

required_capacity_environment=(
  ASTRA_BOOK_CAPACITY_PROFILE
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256
  ASTRA_ORDER_DIRECT_SLOTS
  ASTRA_ORDER_FALLBACK_BUCKETS
  ASTRA_PRICE_PAGE_CAPACITY
  ASTRA_PROFILED_MAX_ORDER_REF
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES
  ASTRA_MIN_DIRECT_ORDER_HEADROOM
  ASTRA_MIN_PRICE_PAGE_HEADROOM
)
for variable_name in "${required_capacity_environment[@]}"; do
  if [[ -z "${!variable_name-}" ]]; then
    echo "${variable_name} is required." >&2
    echo "Load every value from one checksum-backed capacity manifest." >&2
    exit 2
  fi
done

warn_cpu_numa_mismatch() {
  local cpu="$1"

  if [[ -z "${cpu}" || -z "${NUMA_NODE}" ]]; then
    return
  fi

  if ! command -v lscpu >/dev/null 2>&1; then
    return
  fi

  local cpu_node
  cpu_node="$(
    lscpu -p=CPU,NODE 2>/dev/null |
      awk -F, -v cpu="${cpu}" '$1 !~ /^#/ && $1 == cpu {print $2; exit}'
  )"

  if [[ -n "${cpu_node}" && "${cpu_node}" != "${NUMA_NODE}" ]]; then
    echo "Warning: ASTRA_CPU=${cpu} is on NUMA node ${cpu_node}, not ${NUMA_NODE}." >&2
  fi
}

build_numa_command() {
  local cpu="${1:-}"
  NUMA_CMD=()

  if [[ -z "${NUMA_NODE}" ]]; then
    return
  fi

  if ! command -v numactl >/dev/null 2>&1; then
    echo "ASTRA_NUMA_NODE is set, but numactl was not found." >&2
    exit 1
  fi

  NUMA_CMD=(numactl)
  if [[ -n "${cpu}" ]]; then
    warn_cpu_numa_mismatch "${cpu}"
    NUMA_CMD+=("--physcpubind=${cpu}")
  else
    NUMA_CMD+=("--cpunodebind=${NUMA_NODE}")
  fi

  case "${NUMA_MEM_POLICY}" in
    membind)
      NUMA_CMD+=("--membind=${NUMA_NODE}")
      ;;
    localalloc)
      NUMA_CMD+=(--localalloc)
      ;;
    preferred)
      NUMA_CMD+=("--preferred=${NUMA_NODE}")
      ;;
    none)
      ;;
    *)
      echo "Unknown ASTRA_NUMA_MEM_POLICY: ${NUMA_MEM_POLICY}" >&2
      echo "Expected membind, localalloc, preferred, or none." >&2
      exit 2
      ;;
  esac
}

case "${SKIP_BUILD}" in
  0|false|off|no)
    cmake_args=(
      -S "${ROOT_DIR}"
      -B "${BUILD_DIR}"
      -DASTRA_BUILD_APPS=ON
      -DASTRA_BUILD_TESTS=ON
      -DASTRA_BUILD_BENCHMARKS=OFF
      -DASTRA_ENABLE_DPDK=ON
      "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
      "-DASTRA_ENABLE_IPO=${ASTRA_ENABLE_IPO:-OFF}"
    )

    echo "Configuring md_engine with DPDK (build_type=${BUILD_TYPE})..."
    cmake "${cmake_args[@]}"
    cmake --build "${BUILD_DIR}" --target md_engine \
      -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"
    ;;
  1|true|on|yes)
    if [[ ! -x "${BINARY}" ]]; then
      echo "ASTRA_DPDK_SKIP_BUILD is on, but md_engine is missing: ${BINARY}" >&2
      exit 1
    fi
    echo "Using prebuilt DPDK md_engine: ${BINARY}"
    ;;
  *)
    echo "ASTRA_DPDK_SKIP_BUILD must be on/off, true/false, yes/no, or 1/0." >&2
    exit 2
    ;;
esac

git_sha="$(git -C "${ROOT_DIR}" rev-parse --short=12 HEAD 2>/dev/null || true)"
git_sha="${git_sha:-unknown}"
worktree_state="clean"
if [[ -n "$(git -C "${ROOT_DIR}" status --porcelain 2>/dev/null || true)" ]]; then
  worktree_state="dirty"
fi
echo "Build provenance: git_sha=${git_sha} worktree=${worktree_state} build_type=${BUILD_TYPE} ipo=${ASTRA_ENABLE_IPO:-OFF}"

build_numa_command "${ASTRA_CPU:-}"
export ASTRA_RX=dpdk
export ASTRA_DPDK_FLOW_FILTER="${ASTRA_DPDK_FLOW_FILTER:-off}"

echo "Starting DPDK engine: ${ENGINE_ARGS[*]}"
echo "  capacity_profile=${ASTRA_BOOK_CAPACITY_PROFILE}"
echo "  dpdk_port=${ASTRA_DPDK_PORT_ID:-0} dpdk_queue=${ASTRA_DPDK_QUEUE_ID:-0} dpdk_burst=${ASTRA_DPDK_BURST_SIZE:-32}"
echo "  dpdk_latency_mode=${ASTRA_DPDK_LATENCY_MODE:-packet}"
echo "  dpdk_flow_filter=${ASTRA_DPDK_FLOW_FILTER}"
if [[ -n "${ASTRA_DPDK_EAL_ARGS:-}" ]]; then
  echo "  dpdk_eal_args=${ASTRA_DPDK_EAL_ARGS}"
fi
if [[ -n "${NUMA_NODE}" ]]; then
  echo "  numa_node=${NUMA_NODE} numa_mem_policy=${NUMA_MEM_POLICY}"
fi

exec "${NUMA_CMD[@]}" "${BINARY}" "${ENGINE_ARGS[@]}"
