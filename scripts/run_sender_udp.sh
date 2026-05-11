#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/exchange_simulator"

DEST_IP="${1:-127.0.0.1}"
PORT="${2:-9001}"
MPS="${3:-1000}"

if [[ ! -x "${BINARY}" ]]; then
  echo "exchange_simulator not found — configuring and building..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DASTRA_BUILD_APPS=ON
  cmake --build "${BUILD_DIR}" --target exchange_simulator -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"
fi

echo "Starting exchange simulator -> ${DEST_IP}:${PORT} at ${MPS} msg/s"
exec "${BINARY}" "${DEST_IP}" "${PORT}" "${MPS}"
