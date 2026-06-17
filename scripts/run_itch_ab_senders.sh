#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/itch_moldudp_sender"

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

if [[ ! -x "${BINARY}" ]]; then
  echo "itch_moldudp_sender not found; configuring and building..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DASTRA_BUILD_APPS=ON
  cmake --build "${BUILD_DIR}" --target itch_moldudp_sender -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"
fi

if [[ ! -f "${ITCH_FILE}" ]]; then
  echo "ITCH file not found: ${ITCH_FILE}" >&2
  exit 1
fi

pids=()

cleanup() {
  local status=$?
  trap - INT TERM EXIT
  for pid in "${pids[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill -INT "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${pids[@]}"; do
    wait "${pid}" 2>/dev/null || true
  done
  exit "${status}"
}

trap cleanup INT TERM EXIT

echo "Starting ITCH A/B senders"
echo "  file=${ITCH_FILE}"
echo "  line_a=${DEST_IP}:${PORT_A}"
echo "  line_b=${DEST_IP}:${PORT_B}"
echo "  msgs_per_packet=${MSGS_PER_PACKET} session='${SESSION}' rate=${PKTS_PER_SECOND} pkt/s per line"
echo "  premarket_seconds=${PREMARKET_SECONDS}"
echo "  ss_pause_seconds=${SS_PAUSE_SECONDS}"
if [[ -n "${PREMARKET_REPLAY_MODE}" ]]; then
  echo "  premarket_replay_mode=${PREMARKET_REPLAY_MODE} premarket_speedup=${PREMARKET_SPEEDUP}"
fi
if [[ -n "${CPU_A}" || -n "${CPU_B}" ]]; then
  echo "  cpu_a=${CPU_A:-unset} cpu_b=${CPU_B:-unset}"
fi
echo "  press Ctrl+C to stop both"

if [[ -n "${CPU_A}" ]]; then
  ASTRA_CPU="${CPU_A}" "${BINARY}" "${ITCH_FILE}" "${DEST_IP}" "${PORT_A}" "${MSGS_PER_PACKET}" "${SESSION}" "${PKTS_PER_SECOND}" "${PREMARKET_SECONDS}" "${SS_PAUSE_SECONDS}" "${PREMARKET_REPLAY_MODE}" "${PREMARKET_SPEEDUP}" &
else
  "${BINARY}" "${ITCH_FILE}" "${DEST_IP}" "${PORT_A}" "${MSGS_PER_PACKET}" "${SESSION}" "${PKTS_PER_SECOND}" "${PREMARKET_SECONDS}" "${SS_PAUSE_SECONDS}" "${PREMARKET_REPLAY_MODE}" "${PREMARKET_SPEEDUP}" &
fi
pids+=("$!")

if [[ -n "${CPU_B}" ]]; then
  ASTRA_CPU="${CPU_B}" "${BINARY}" "${ITCH_FILE}" "${DEST_IP}" "${PORT_B}" "${MSGS_PER_PACKET}" "${SESSION}" "${PKTS_PER_SECOND}" "${PREMARKET_SECONDS}" "${SS_PAUSE_SECONDS}" "${PREMARKET_REPLAY_MODE}" "${PREMARKET_SPEEDUP}" &
else
  "${BINARY}" "${ITCH_FILE}" "${DEST_IP}" "${PORT_B}" "${MSGS_PER_PACKET}" "${SESSION}" "${PKTS_PER_SECOND}" "${PREMARKET_SECONDS}" "${SS_PAUSE_SECONDS}" "${PREMARKET_REPLAY_MODE}" "${PREMARKET_SPEEDUP}" &
fi
pids+=("$!")

wait -n "${pids[@]}"
cleanup
