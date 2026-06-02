#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/md_engine"

LISTEN_IP="${1:-127.0.0.1}"
PORT="${2:-9001}"

if [[ ! -x "${BINARY}" ]]; then
  echo "md_engine not found — configuring and building..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DASTRA_BUILD_APPS=ON
  cmake --build "${BUILD_DIR}" --target md_engine -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"
fi

echo "Starting engine on ${LISTEN_IP}:${PORT} — press Ctrl+C to stop"
exec "${BINARY}" "${LISTEN_IP}" "${PORT}"
