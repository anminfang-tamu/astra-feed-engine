#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ASTRA_DPDK_BUILD_DIR:-${ROOT_DIR}/build-dpdk}"
BINARY="${BUILD_DIR}/md_engine"
BUILD_TYPE="${ASTRA_BUILD_TYPE:-Release}"

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

Useful environment:
  ASTRA_DPDK_PORT_ID=0
  ASTRA_DPDK_EAL_ARGS="--main-lcore 2 -l 2"
  ASTRA_DPDK_BURST_SIZE=32  # must be divisible by 8
  ASTRA_DPDK_LATENCY_MODE=packet  # packet (default) or burst
  ASTRA_DPDK_FLOW_FILTER=on  # on (default) or off
  ASTRA_DPDK_RX_DESC=4096
  ASTRA_DPDK_MEMPOOL_SIZE=65535
  ASTRA_DPDK_PROMISCUOUS=off
  ASTRA_DPDK_ALLMULTICAST=on
  ASTRA_BOOK_ORDER_CAPACITY=65536  # default-tier symbols
  ASTRA_LATENCY_METRICS=on
  ASTRA_LATENCY_PERCENTILES_ONLY=on
  ASTRA_BOOK_STATS=off  # suppress verbose shutdown book/pool diagnostics
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

cmake_args=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -DASTRA_BUILD_APPS=ON
  -DASTRA_ENABLE_DPDK=ON
  "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  "-DASTRA_ENABLE_IPO=${ASTRA_ENABLE_IPO:-OFF}"
)

echo "Configuring md_engine with DPDK (build_type=${BUILD_TYPE})..."
cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --target md_engine \
  -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

git_sha="$(git -C "${ROOT_DIR}" rev-parse --short=12 HEAD 2>/dev/null || true)"
git_sha="${git_sha:-unknown}"
worktree_state="clean"
if [[ -n "$(git -C "${ROOT_DIR}" status --porcelain 2>/dev/null || true)" ]]; then
  worktree_state="dirty"
fi
echo "Build provenance: git_sha=${git_sha} worktree=${worktree_state} build_type=${BUILD_TYPE} ipo=${ASTRA_ENABLE_IPO:-OFF}"

build_numa_command "${ASTRA_CPU:-}"
export ASTRA_RX=dpdk

echo "Starting DPDK engine: ${ENGINE_ARGS[*]}"
echo "  dpdk_port=${ASTRA_DPDK_PORT_ID:-0} dpdk_burst=${ASTRA_DPDK_BURST_SIZE:-32}"
echo "  dpdk_latency_mode=${ASTRA_DPDK_LATENCY_MODE:-packet}"
echo "  dpdk_flow_filter=${ASTRA_DPDK_FLOW_FILTER:-on}"
if [[ -n "${ASTRA_DPDK_EAL_ARGS:-}" ]]; then
  echo "  dpdk_eal_args=${ASTRA_DPDK_EAL_ARGS}"
fi
if [[ -n "${NUMA_NODE}" ]]; then
  echo "  numa_node=${NUMA_NODE} numa_mem_policy=${NUMA_MEM_POLICY}"
fi

exec "${NUMA_CMD[@]}" "${BINARY}" "${ENGINE_ARGS[@]}"
