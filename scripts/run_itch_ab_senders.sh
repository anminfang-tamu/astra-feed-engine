#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/itch_moldudp_sender"
BUILD_TYPE="${ASTRA_BUILD_TYPE:-Release}"

ITCH_FILE="${1:-${ROOT_DIR}/data/itch/unzipped/01302019.NASDAQ_ITCH50}"
DEST_IP="${2:-127.0.0.1}"
PORT_A="${3:-9000}"
PORT_B="${4:-9001}"
MSGS_PER_PACKET="${5:-20}"
SESSION="${6:-ASTRA     }"
PKTS_PER_SECOND="${7:-5000}"
PREMARKET_SECONDS="${8:-${ASTRA_PREMARKET_SECONDS:-0}}"
SS_PAUSE_SECONDS="${9:-${ASTRA_SS_PAUSE_SECONDS:-0}}"
PREMARKET_REPLAY_MODE="${10:-${ASTRA_PREMARKET_REPLAY_MODE:-}}"
PREMARKET_SPEEDUP="${11:-${ASTRA_PREMARKET_SPEEDUP:-1}}"
CPU_A="${ASTRA_CPU_A:-}"
CPU_B="${ASTRA_CPU_B:-}"
LINE_B_DELAY_NS="${ASTRA_LINE_B_DELAY_NS:-1000}"
NUMA_NODE="${ASTRA_NUMA_NODE:-}"
NUMA_MEM_POLICY="${ASTRA_NUMA_MEM_POLICY:-membind}"

if [[ -n "${CPU_A}" || -n "${CPU_B}" ]]; then
  if [[ -z "${CPU_A}" || -z "${CPU_B}" ]]; then
    echo "ASTRA_CPU_A and ASTRA_CPU_B must either both be set or both be unset." >&2
    exit 2
  fi
  if [[ ! "${CPU_A}" =~ ^[0-9]+$ || ! "${CPU_B}" =~ ^[0-9]+$ ]]; then
    echo "ASTRA_CPU_A and ASTRA_CPU_B must be non-negative integer CPU IDs." >&2
    exit 2
  fi
  if ((10#${CPU_A} == 10#${CPU_B})); then
    echo "ASTRA_CPU_A and ASTRA_CPU_B must identify different CPUs." >&2
    exit 2
  fi
  export ASTRA_CPU_A="${CPU_A}"
  export ASTRA_CPU_B="${CPU_B}"
fi

if [[ ! "${LINE_B_DELAY_NS}" =~ ^[0-9]+$ ]]; then
  echo "ASTRA_LINE_B_DELAY_NS must be a non-negative integer number of nanoseconds." >&2
  exit 2
fi
if ((${#LINE_B_DELAY_NS} > 10)) ||
  ((${#LINE_B_DELAY_NS} == 10 && 10#${LINE_B_DELAY_NS} > 1000000000)); then
  echo "ASTRA_LINE_B_DELAY_NS must not exceed 1000000000 nanoseconds." >&2
  exit 2
fi
export ASTRA_LINE_B_DELAY_NS="${LINE_B_DELAY_NS}"

echo "Configuring synchronized ITCH A/B feeder (build_type=${BUILD_TYPE})..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=ON \
  -DASTRA_BUILD_BENCHMARKS=OFF \
  -DASTRA_ENABLE_DPDK=OFF \
  "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" \
  "-DASTRA_ENABLE_IPO=${ASTRA_ENABLE_IPO:-OFF}"
cmake --build "${BUILD_DIR}" --target itch_moldudp_sender \
  -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

git_sha="$(git -C "${ROOT_DIR}" rev-parse --short=12 HEAD 2>/dev/null || true)"
git_sha="${git_sha:-unknown}"
worktree_state="clean"
if [[ -n "$(git -C "${ROOT_DIR}" status --porcelain 2>/dev/null || true)" ]]; then
  worktree_state="dirty"
fi
echo "Build provenance: git_sha=${git_sha} worktree=${worktree_state} build_type=${BUILD_TYPE} ipo=${ASTRA_ENABLE_IPO:-OFF}"

if [[ ! -f "${ITCH_FILE}" ]]; then
  echo "ITCH file not found: ${ITCH_FILE}" >&2
  exit 1
fi

warn_cpu_numa_mismatch() {
  local variable_name="$1"
  local cpu="$2"

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
    echo "Warning: ${variable_name}=${cpu} is on NUMA node ${cpu_node}, not ${NUMA_NODE}." >&2
  fi
}

build_numa_command() {
  NUMA_CMD=()

  if [[ -z "${NUMA_NODE}" ]]; then
    return
  fi

  if ! command -v numactl >/dev/null 2>&1; then
    echo "ASTRA_NUMA_NODE is set, but numactl was not found." >&2
    exit 1
  fi

  NUMA_CMD=(numactl)
  if [[ -n "${CPU_A}" || -n "${CPU_B}" ]]; then
    local cpu_mask=""
    if [[ -n "${CPU_A}" ]]; then
      warn_cpu_numa_mismatch ASTRA_CPU_A "${CPU_A}"
      cpu_mask="${CPU_A}"
    fi
    if [[ -n "${CPU_B}" ]]; then
      warn_cpu_numa_mismatch ASTRA_CPU_B "${CPU_B}"
      cpu_mask="${cpu_mask:+${cpu_mask},}${CPU_B}"
    fi
    NUMA_CMD+=("--physcpubind=${cpu_mask}")
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

build_sender_command() {
  build_numa_command
  SENDER_CMD=("${NUMA_CMD[@]}")
  SENDER_CMD+=(
    "${BINARY}"
    --redundant
    "${ITCH_FILE}"
    "${DEST_IP}"
    "${PORT_A}"
    "${PORT_B}"
    "${MSGS_PER_PACKET}"
    "${SESSION}"
    "${PKTS_PER_SECOND}"
    "${PREMARKET_SECONDS}"
    "${SS_PAUSE_SECONDS}"
    "${PREMARKET_REPLAY_MODE}"
    "${PREMARKET_SPEEDUP}"
  )
}

echo "Starting synchronized ITCH A/B feeder"
echo "  file=${ITCH_FILE}"
echo "  line_a=${DEST_IP}:${PORT_A}"
echo "  line_b=${DEST_IP}:${PORT_B}"
echo "  msgs_per_packet=${MSGS_PER_PACKET} session='${SESSION}' rate=${PKTS_PER_SECOND} pkt/s per line"
echo "  premarket_seconds=${PREMARKET_SECONDS}"
echo "  ss_pause_seconds=${SS_PAUSE_SECONDS}"
echo "  line_b_delay_ns=${LINE_B_DELAY_NS}"
if [[ -n "${PREMARKET_REPLAY_MODE}" ]]; then
  echo "  premarket_replay_mode=${PREMARKET_REPLAY_MODE} premarket_speedup=${PREMARKET_SPEEDUP}"
fi
if [[ -n "${CPU_A}" || -n "${CPU_B}" ]]; then
  echo "  cpu_a=${CPU_A:-unset} cpu_b=${CPU_B:-unset}"
fi
if [[ -n "${NUMA_NODE}" ]]; then
  echo "  numa_node=${NUMA_NODE} numa_mem_policy=${NUMA_MEM_POLICY}"
fi
echo "  source=one_packet_stream line_threads=2 delivery=synchronized_A_B"
echo "  press Ctrl+C to stop"

SENDER_CMD=()
build_sender_command
exec "${SENDER_CMD[@]}"
