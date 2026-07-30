#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dpdk_ec2_config.sh
source "${SCRIPT_DIR}/dpdk_ec2_config.sh"

usage() {
  cat <<'USAGE'
Usage: ./scripts/dpdk/dpdk_02_build.sh

Configure a Release build with DPDK enabled and build only md_engine.
Tests, benchmarks, trace tools, and sender workflows are not built.
Then validate the selected capacity plan against NUMA and cgroup memory before
the feed ENI can be bound in step 3.
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

if [[ "$(uname -s)" != Linux ]]; then
  echo "The DPDK build is Linux-only." >&2
  exit 1
fi
for command_name in awk cmake dirname grep mkdir mktemp mv nproc pkg-config \
                    rm sha256sum; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "Missing required command: ${command_name}" >&2
    exit 1
  fi
done
if [[ ! -f "${ASTRA_REPO_ROOT}/CMakeLists.txt" ]]; then
  echo "CMakeLists.txt was not found at ${ASTRA_REPO_ROOT}." >&2
  exit 1
fi

pkg-config --exists libdpdk || {
  echo "pkg-config cannot find libdpdk; run scripts/dpdk/dpdk_01_install.sh first." >&2
  exit 1
}

cmake -S "${ASTRA_REPO_ROOT}" -B "${ASTRA_REPO_ROOT}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_BUILD_APPS=ON \
  -DASTRA_BUILD_TESTS=OFF \
  -DASTRA_BUILD_BENCHMARKS=OFF \
  -DASTRA_ENABLE_DPDK=ON \
  -DASTRA_ENABLE_IPO=OFF

cmake --build "${ASTRA_REPO_ROOT}/build" \
  --target md_engine \
  -j"$(nproc)"

if [[ ! -x "${ASTRA_REPO_ROOT}/build/md_engine" ]]; then
  echo "DPDK md_engine was not produced." >&2
  exit 1
fi
if ! grep -Fxq 'ASTRA_ENABLE_DPDK:BOOL=ON' \
    "${ASTRA_REPO_ROOT}/build/CMakeCache.txt"; then
  echo "The build cache does not have ASTRA_ENABLE_DPDK enabled." >&2
  exit 1
fi

if [[ ! "${ASTRA_FEED_NUMA}" =~ ^[0-9]+$ ]] ||
    ! astra_is_valid_hugepage_count "${ASTRA_DPDK_HUGE_PAGES}"; then
  echo "Feed NUMA node or 2 MiB hugepage count is invalid." >&2
  exit 2
fi
astra_load_book_capacity
CAPACITY_PLAN_OUTPUT="$(
  "${ASTRA_REPO_ROOT}/build/md_engine" --book-storage-plan-only
)"
awk '/^book_storage_plan / {print}' <<<"${CAPACITY_PLAN_OUTPUT}"
PLANNED_STORAGE_BYTES="$(
  awk '
    /^book_storage_plan / {
      for (i=2; i<=NF; ++i) {
        if ($i ~ /^planned_storage_bytes=/) {
          sub(/^planned_storage_bytes=/, "", $i)
          print $i
          found=1
        }
      }
    }
    END {if (!found) exit 1}
  ' <<<"${CAPACITY_PLAN_OUTPUT}"
)"
if ! astra_is_uint63 "${PLANNED_STORAGE_BYTES}"; then
  echo "md_engine returned an invalid planned storage size." >&2
  exit 1
fi

HUGEPAGE_BYTES=$((ASTRA_DPDK_HUGE_PAGES * 2 * 1024 * 1024))
ENGINE_RESERVE_BYTES=$((16 * 1024 * 1024 * 1024))
if (( PLANNED_STORAGE_BYTES >
      9223372036854775807 - HUGEPAGE_BYTES - ENGINE_RESERVE_BYTES )); then
  echo "The capacity plan exceeds signed 64-bit admission arithmetic." >&2
  exit 1
fi
REQUIRED_AVAILABLE_BYTES=$((
  PLANNED_STORAGE_BYTES + HUGEPAGE_BYTES + ENGINE_RESERVE_BYTES
))
astra_check_capacity_memory \
  "$((PLANNED_STORAGE_BYTES + ENGINE_RESERVE_BYTES))" \
  "${HUGEPAGE_BYTES}"

ENGINE_SHA256="$(
  sha256sum "${ASTRA_REPO_ROOT}/build/md_engine" |
    awk '{print $1}'
)"
ADMISSION_DIR="$(dirname "${ASTRA_DPDK_ADMISSION_FILE}")"
mkdir -p "${ADMISSION_DIR}"
umask 077
ADMISSION_TEMP="$(mktemp "${ASTRA_DPDK_ADMISSION_FILE}.tmp.XXXXXX")"
cleanup_admission_temp() {
  rm -f -- "${ADMISSION_TEMP}"
}
trap cleanup_admission_temp EXIT
{
  printf 'ASTRA_ADMITTED_CAPACITY_FILE=%q\n' \
    "${ASTRA_BOOK_CAPACITY_FILE}"
  printf 'ASTRA_ADMITTED_CAPACITY_SHA256=%q\n' \
    "${ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256}"
  printf 'ASTRA_ADMITTED_ENGINE_SHA256=%q\n' "${ENGINE_SHA256}"
  printf 'ASTRA_ADMITTED_FEED_NUMA=%q\n' "${ASTRA_FEED_NUMA}"
  printf 'ASTRA_ADMITTED_HUGE_PAGES=%q\n' "${ASTRA_DPDK_HUGE_PAGES}"
  printf 'ASTRA_ADMITTED_PLANNED_BYTES=%q\n' "${PLANNED_STORAGE_BYTES}"
  printf 'ASTRA_ADMITTED_REQUIRED_BYTES=%q\n' "${REQUIRED_AVAILABLE_BYTES}"
} >"${ADMISSION_TEMP}"
mv -f -- "${ADMISSION_TEMP}" "${ASTRA_DPDK_ADMISSION_FILE}"
trap - EXIT

echo "Step 2 complete: ${ASTRA_REPO_ROOT}/build/md_engine"
echo "Capacity admission: ${ASTRA_DPDK_ADMISSION_FILE}"
