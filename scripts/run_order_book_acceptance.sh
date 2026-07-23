#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

DEFAULT_BINARY="${ROOT_DIR}/build/benchmarks/astra_itch_book_replay_benchmark"
DEFAULT_RESERVE_BYTES=17179869184
BRANCH5_PINNED_TRACE_SHA256=1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3
BRANCH5_PINNED_DEFAULT_ORDER_CAPACITY=65536
BRANCH5_PINNED_PRICE_NODE_CAPACITY=163840
BRANCH5_PINNED_PRICE_LEAF_CAPACITY=1048576
BRANCH5_PINNED_PRICE_LEVEL_CAPACITY=2097152
BRANCH5_PINNED_PRICE_POOL_BYTES=2456420352
BRANCH5_PINNED_PREPARED_BOOKS=8713
BRANCH5_PINNED_NATIVE_RANGE_COUNT=34858
BRANCH5_PINNED_NATIVE_RANGE_BYTES=62404952064
BRANCH5_PINNED_SAMPLE_EVERY=64
BRANCH5_PINNED_SAMPLE_CAPACITY=8388608
BRANCH5_PINNED_RESERVE_BYTES=17179869184
# The exact native vector payload is 58.119140625 GiB. A canonical 64 GiB
# admission plan leaves 5.880859375 GiB for allocator/page rounding before the
# separately admitted 16 GiB runtime/OS reserve.
BRANCH5_PINNED_PLANNED_BYTES=68719476736
BRANCH5_PINNED_CAPACITY_PROFILE_NAME=nasdaq-itch-20190130-branch5-native-v1
BRANCH5_PINNED_CAPACITY_EVIDENCE_RELATIVE=compat/branch5_native_v1/capacity-evidence-01302019.txt
BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256=05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282

BINARY="${DEFAULT_BINARY}"
TRACE=""
OUTPUT_DIR=""
EXPECTED_HOT_ARENA_SCHEMA=""
RUNS=5
CPU=""
NUMA_NODE=""
MONITOR_CPU=""
EXPECTED_RECORDS=""
EXPECTED_BYTES=""
MAX_P50_NS=""
MAX_P99_NS=""
MAX_P999_NS=""
SAMPLE_EVERY=64
WARMUP_BOOK_MESSAGES=1000000
MIN_SAMPLES=1000
PLANNED_BYTES_OVERRIDE=""
PLANNED_BYTES=""
RESERVE_BYTES="${DEFAULT_RESERVE_BYTES}"
DIRECT_ORDER_SLOTS=""
FALLBACK_BUCKETS=""
PRICE_PAGE_CAPACITY=""
CAPACITY_PROFILE_NAME=""
CAPACITY_EVIDENCE_FILE=""
CAPACITY_EVIDENCE_SHA256=""
DEFAULT_ORDER_CAPACITY=""
PRICE_NODE_CAPACITY=""
PRICE_LEAF_CAPACITY=""
PRICE_LEVEL_CAPACITY=""
SAMPLE_CAPACITY=""
DISCOVER_DIGEST=0
EXPECTED_MUTATION_DIGEST=""
EXPECTED_SEMANTIC_MUTATION_DIGEST=""
ALLOW_SWAP=0
RUN_PERF_STAT=1
HASH_TRACE=0
DRY_RUN=0
NUMACTL_BIN=numactl
PERF_BIN=perf
PERF_EVENTS=cycles,instructions,branches,branch-misses,cache-misses,dTLB-loads,dTLB-load-misses,page-faults,context-switches,cpu-migrations
SAMPLE_SCHEDULE_ID=fixed_block_offset_v1_splitmix64_seed_61737472612d6974
ARENA_ALIGNMENT_BYTES=2097152
HOT_ARENA_SCHEMA=""
ARENA_IDS=()
ARENA_VMA_NAMES=()
ARENA_MAPPED_SUBTOTAL_FLAGS=()
ARENA_DIRECT_INDEX=-1
ARENA_DESCRIPTOR_INDEX=-1
ARENA_PRICE_PAGES_INDEX=-1
HOT_ARENA_POLICY=""
BRANCH5_CAPACITY_POLICY=not_applicable
BRANCH5_CAPACITY_PROFILE_NAME=not_applicable
BRANCH5_CAPACITY_EVIDENCE_POLICY=not_applicable
BRANCH5_RUNTIME_CAPACITY_MODE=not_applicable
CAPACITY_EVIDENCE_PROVENANCE_SOURCE=""
CAPACITY_EVIDENCE_EXPECTED_SHA256=not_applicable
ACTIVE_COMMAND_PID=""
ACTIVE_MONITOR_PID=""
SOURCE_BUILD_TEMP_ROOT=""
ARTIFACTS_INITIALIZED=0
MANIFEST_FINALIZED=0

finalize_artifact_manifest() {
  local manifest_tmp=""
  if [[ "${ARTIFACTS_INITIALIZED}" -ne 1 ||
        "${MANIFEST_FINALIZED}" -eq 1 ||
        ! -d "${OUTPUT_DIR}" ]] ||
     ! command -v sha256sum >/dev/null 2>&1; then
    return 0
  fi

  manifest_tmp="${OUTPUT_DIR}/.manifest.sha256.tmp"
  if ! (
    cd -- "${OUTPUT_DIR}"
    find . -type f \
      ! -path ./manifest.sha256 \
      ! -path ./.manifest.sha256.tmp \
      -print0 |
      sort -z |
      xargs -0 -r sha256sum
  ) >"${manifest_tmp}"; then
    rm -f -- "${manifest_tmp}"
    return 1
  fi
  if ! mv -- "${manifest_tmp}" "${OUTPUT_DIR}/manifest.sha256"; then
    rm -f -- "${manifest_tmp}"
    return 1
  fi
  MANIFEST_FINALIZED=1
  return 0
}

cleanup_active_children() {
  local child_pid=""
  for child_pid in "${ACTIVE_COMMAND_PID}" "${ACTIVE_MONITOR_PID}"; do
    if [[ "${child_pid}" =~ ^[0-9]+$ ]] && kill -0 "${child_pid}" 2>/dev/null; then
      kill -TERM "${child_pid}" 2>/dev/null || true
    fi
  done
  for child_pid in "${ACTIVE_COMMAND_PID}" "${ACTIVE_MONITOR_PID}"; do
    if [[ "${child_pid}" =~ ^[0-9]+$ ]]; then
      wait "${child_pid}" 2>/dev/null || true
    fi
  done
  ACTIVE_COMMAND_PID=""
  ACTIVE_MONITOR_PID=""
}

cleanup_source_build_temp() {
  local temp_basename=""
  if [[ -z "${SOURCE_BUILD_TEMP_ROOT}" ]]; then
    return 0
  fi
  temp_basename="$(basename -- "${SOURCE_BUILD_TEMP_ROOT}")"
  if [[ ! -d "${SOURCE_BUILD_TEMP_ROOT}" ||
        "${temp_basename}" != astra-order-book-source-build.* ]]; then
    echo "acceptance harness: refusing unsafe source-build cleanup target: ${SOURCE_BUILD_TEMP_ROOT}" >&2
    return 1
  fi
  rm -rf -- "${SOURCE_BUILD_TEMP_ROOT}"
  SOURCE_BUILD_TEMP_ROOT=""
}

handle_exit() {
  local status=$?
  trap - EXIT
  cleanup_active_children
  if ! cleanup_source_build_temp; then
    status=1
  fi
  if ! finalize_artifact_manifest; then
    echo "acceptance harness: could not finalize artifact manifest" >&2
  fi
  exit "${status}"
}

trap handle_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

usage() {
  cat <<'USAGE'
Usage: scripts/run_order_book_acceptance.sh [options]

Run the full parser/book replay repeatedly on a pinned EC2 CPU and NUMA node.
Every requested candidate run must pass all three supplied latency ceilings;
the harness never selects the best run. stdout, stderr, commands, preflight
evidence, per-run metrics, and memory-placement snapshots are retained.

Required:
  --trace FILE                    Length-prefixed ITCH input.
  --cpu N                         Single isolated logical CPU.
  --numa-node N                   NUMA memory node local to --cpu.
  --expect-records N              Exact replay record count.
  --expect-bytes N                Exact replay byte count.
  --max-p50-ns N                  Candidate p50 ceiling.
  --max-p99-ns N                  Candidate p99 ceiling.
  --max-p99-9-ns N                Candidate p99.9 ceiling.

Options:
  --runs N                        Number of candidate processes; default and
                                  minimum: 5.
  --binary FILE                   Replay binary; default:
                                  build/benchmarks/astra_itch_book_replay_benchmark
  --expect-hot-arena-schema NAME  Expected binary storage schema:
                                  redesign_v1 or branch5_native_v1. Required
                                  with --dry-run because files are not probed;
                                  live runs fail if the binary reports another
                                  schema.
  --monitor-cpu N                 Housekeeping CPU for the short /proc memory
                                  monitor. Default: CPU 0, or CPU 1 when the
                                  candidate uses CPU 0.
  --output-dir DIR                New artifact directory. Existing paths are
                                  never reused or removed.
  --sample-every N                Fixed-seed block size; default: 64.
  --warmup-book-messages N        Untimed book-message warmup; default: 1000000.
  --min-samples N                 Minimum aggregate samples; default: 1000.
  --direct-order-slots N          Override the replay's direct-order capacity.
  --fallback-buckets N            Override fallback bucket count.
  --price-page-capacity N         Override price-page capacity.
  --capacity-profile-name NAME    Custom redesign capacity-profile name.
  --capacity-evidence-file FILE   Canonical custom capacity evidence manifest.
  --capacity-evidence-sha256 HEX  Independently approved SHA-256 of FILE.
  --default-order-capacity N      Override branch5 per-book order capacity.
  --price-node-capacity N         Override branch5 internal-node capacity.
  --price-leaf-capacity N         Override branch5 leaf capacity.
  --price-level-capacity N        Override branch5 price-level capacity.
  --sample-capacity N             Override pre-resident timed-sample capacity.
  --planned-bytes N               Optional conservative admission footprint.
                                  Must be at least the binary's storage plan;
                                  otherwise the plan is used directly. Required
                                  for branch5_native_v1 because per-book vector
                                  bytes resolve only after the R/S prelude.
  --reserve-bytes N               Required node/cgroup headroom in addition to
                                  planned bytes; default: 17179869184 (16 GiB).
  --correctness-digest            After latency runs, run one digest discovery
                                  process and a second verification process.
  --expect-mutation-digest N      Instead run one separate correctness process
                                  against this known digest.
  --expect-semantic-mutation-digest N
                                  Also gate the independent semantic digest.
  --allow-swap                    Record swap state but do not require SwapTotal=0.
  --no-perf-stat                  Explicitly skip the separate hardware-counter
                                  process. Full acceptance runs retain it by
                                  default.
  --hash-trace                    Deprecated compatibility no-op. Live runs
                                  always hash the trace before and after replay.
  --dry-run                       Validate CLI and print all planned commands.
                                  Does not require Linux, files, numactl, or
                                  create an output directory.
  -h, --help                      Show this help.

This is a single-variant harness: it evaluates the supplied binary against the
three supplied ceilings and does not run or compare another branch. Digest
observation is always run in a separate process without latency gates.
Accepted runs require an explicit supported hot-arena schema. `redesign_v1`
requires transparent huge pages plus exact 2 MiB alignment, VMA identity,
whole-extent huge-page backing, and requested-node residency for all eleven
named arenas. `branch5_native_v1` instead validates its sealed native-vector
manifest against anonymous process VMAs and aggregate NUMA placement; it never
pretends those allocator ranges are redesign arenas. Live runs also require a
Release CMake build. The harness exports the verified clean commit, configures
a fresh out-of-tree benchmark-only build graph, and performs a clean-first
target build. The supplied binary must be byte-identical to that rebuilt
target. Trace, binary, harness, and Git state must then remain stable through
the post-run snapshot. The named clean branch must not be detached, and --cpu
must appear in Linux's domain-isolated CPU list; an exposed scaling governor
must be performance.
USAGE
}

die() {
  echo "acceptance harness: $*" >&2
  exit 2
}

need_value() {
  if [[ $# -lt 2 || -z "$2" ]]; then
    die "missing value for $1"
  fi
}

is_uint() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

is_sha256() {
  [[ "$1" =~ ^[0-9a-f]{64}$ ]]
}

configure_hot_arena_schema() {
  local schema="$1"
  HOT_ARENA_SCHEMA="${schema}"
  ARENA_IDS=()
  ARENA_VMA_NAMES=()
  ARENA_MAPPED_SUBTOTAL_FLAGS=()
  ARENA_DIRECT_INDEX=-1
  ARENA_DESCRIPTOR_INDEX=-1
  ARENA_PRICE_PAGES_INDEX=-1
  HOT_ARENA_POLICY=""
  ARENA_ALIGNMENT_BYTES=0

  case "${schema}" in
    redesign_v1)
      ARENA_ALIGNMENT_BYTES=2097152
      ARENA_IDS=(
        order_direct
        order_fallback
        book_descriptors
        price_roots
        price_prepared_books
        price_pages
        price_page_owners
        price_page_summaries
        price_page_occupancy
        price_book_summaries
        price_book_occupancy
      )
      ARENA_VMA_NAMES=(
        astra-order-direct
        astra-order-fallback
        astra-book-descriptors
        astra-price-roots
        astra-price-prepared
        astra-price-pages
        astra-price-owners
        astra-price-summaries
        astra-price-page-bitmap
        astra-price-book-summaries
        astra-price-book-bitmap
      )
      # The descriptor mapping is reported separately from the legacy
      # mapped_array_bytes subtotal. Every other redesign_v1 arena contributes
      # to that subtotal.
      ARENA_MAPPED_SUBTOTAL_FLAGS=(1 1 0 1 1 1 1 1 1 1 1)
      ARENA_DIRECT_INDEX=0
      ARENA_DESCRIPTOR_INDEX=2
      ARENA_PRICE_PAGES_INDEX=5
      HOT_ARENA_POLICY=redesign_exact_v1
      ;;
    branch5_native_v1)
      # Branch 5 owns allocator-backed vectors, not the redesign's eleven
      # aligned mappings. Its exact payload spans are declared only after the
      # trace's directory prelude prepares and seals the native book universe.
      HOT_ARENA_POLICY=branch5_native_ranges_v1
      ;;
    *)
      return 1
      ;;
  esac

  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      [[ "${#ARENA_IDS[@]}" -eq 11 &&
         "${#ARENA_VMA_NAMES[@]}" -eq "${#ARENA_IDS[@]}" &&
         "${#ARENA_MAPPED_SUBTOTAL_FLAGS[@]}" -eq "${#ARENA_IDS[@]}" &&
         "${ARENA_IDS[ARENA_DIRECT_INDEX]}" == order_direct &&
         "${ARENA_IDS[ARENA_DESCRIPTOR_INDEX]}" == book_descriptors &&
         "${ARENA_IDS[ARENA_PRICE_PAGES_INDEX]}" == price_pages ]]
      ;;
    branch5_native_ranges_v1)
      [[ "${#ARENA_IDS[@]}" -eq 0 && "${#ARENA_VMA_NAMES[@]}" -eq 0 ]]
      ;;
    *)
      return 1
      ;;
  esac
}

hot_arena_schema_matches_expectation() {
  [[ -z "$2" || "$1" == "$2" ]]
}

normalize_uint() {
  local value="$1"
  while [[ "${#value}" -gt 1 && "${value:0:1}" == 0 ]]; do
    value="${value:1}"
  done
  printf '%s' "${value}"
}

require_uint() {
  local value="$1"
  local label="$2"
  if ! is_uint "${value}"; then
    die "${label} must be an unsigned decimal integer"
  fi
}

require_positive_uint() {
  local value="$1"
  local label="$2"
  require_uint "${value}" "${label}"
  if [[ "$(normalize_uint "${value}")" == 0 ]]; then
    die "${label} must be greater than zero"
  fi
}

# Decimal-string comparison avoids signed-shell overflow for uint64 fields.
uint_gt() {
  local left
  local right
  left="$(normalize_uint "$1")"
  right="$(normalize_uint "$2")"
  if [[ "${#left}" -ne "${#right}" ]]; then
    [[ "${#left}" -gt "${#right}" ]]
  else
    [[ "${left}" > "${right}" ]]
  fi
}

uint_ge() {
  [[ "$(normalize_uint "$1")" == "$(normalize_uint "$2")" ]] ||
    uint_gt "$1" "$2"
}

branch5_runtime_capacity_request_is_canonical() {
  if [[ -z "$1" && -z "$2" && -z "$3" && -z "$4" ]]; then
    return 0
  fi
  [[ -n "$1" && -n "$2" && -n "$3" && -n "$4" &&
     "$(normalize_uint "$1")" == \
       "${BRANCH5_PINNED_DEFAULT_ORDER_CAPACITY}" &&
     "$(normalize_uint "$2")" == \
       "${BRANCH5_PINNED_PRICE_NODE_CAPACITY}" &&
     "$(normalize_uint "$3")" == \
       "${BRANCH5_PINNED_PRICE_LEAF_CAPACITY}" &&
     "$(normalize_uint "$4")" == \
       "${BRANCH5_PINNED_PRICE_LEVEL_CAPACITY}" ]]
}

branch5_plan_is_canonical() {
  [[ "$(normalize_uint "$1")" == \
       "${BRANCH5_PINNED_DEFAULT_ORDER_CAPACITY}" &&
     "$(normalize_uint "$2")" == \
       "${BRANCH5_PINNED_PRICE_NODE_CAPACITY}" &&
     "$(normalize_uint "$3")" == \
       "${BRANCH5_PINNED_PRICE_LEAF_CAPACITY}" &&
     "$(normalize_uint "$4")" == \
       "${BRANCH5_PINNED_PRICE_LEVEL_CAPACITY}" &&
     "$(normalize_uint "$5")" == \
       "${BRANCH5_PINNED_PRICE_POOL_BYTES}" ]]
}

BRANCH5_CAPACITY_ERROR=""
validate_branch5_capacity_extent() {
  local prepared_books
  local range_count
  local range_bytes
  local planned_bytes
  local policy="$5"
  BRANCH5_CAPACITY_ERROR=""

  prepared_books="$(normalize_uint "$1")"
  range_count="$(normalize_uint "$2")"
  range_bytes="$(normalize_uint "$3")"
  planned_bytes="$(normalize_uint "$4")"

  if uint_gt "${range_bytes}" "${planned_bytes}"; then
    BRANCH5_CAPACITY_ERROR="branch5 native payload exceeds the admitted --planned-bytes bound"
    return 1
  fi
  if [[ "${policy}" == pinned_trace_canonical_v1 ]] &&
     [[ "${prepared_books}" != "${BRANCH5_PINNED_PREPARED_BOOKS}" ||
        "${range_count}" != "${BRANCH5_PINNED_NATIVE_RANGE_COUNT}" ||
        "${range_bytes}" != "${BRANCH5_PINNED_NATIVE_RANGE_BYTES}" ||
        "${planned_bytes}" != "${BRANCH5_PINNED_PLANNED_BYTES}" ]]; then
    BRANCH5_CAPACITY_ERROR="branch5 native extent differs from the pinned full-trace capacity profile"
    return 1
  fi
  return 0
}

# Add unsigned decimal strings without routing uint64 values through Bash's
# signed machine-word arithmetic.
uint_add() {
  local left
  local right
  local left_index
  local right_index
  local left_digit=0
  local right_digit=0
  local carry=0
  local total=0
  local result=""
  left="$(normalize_uint "$1")"
  right="$(normalize_uint "$2")"
  left_index="${#left}"
  right_index="${#right}"

  while [[ "${left_index}" -gt 0 || "${right_index}" -gt 0 ||
           "${carry}" -ne 0 ]]; do
    left_digit=0
    right_digit=0
    if [[ "${left_index}" -gt 0 ]]; then
      left_index=$((left_index - 1))
      left_digit="${left:${left_index}:1}"
    fi
    if [[ "${right_index}" -gt 0 ]]; then
      right_index=$((right_index - 1))
      right_digit="${right:${right_index}:1}"
    fi
    total=$((left_digit + right_digit + carry))
    result="$((total % 10))${result}"
    carry=$((total / 10))
  done
  printf '%s' "${result:-0}"
}

# Subtract unsigned decimal strings without signed machine-word arithmetic.
# The caller must provide left >= right.
uint_subtract() {
  local left
  local right
  local left_index
  local right_index
  local left_digit=0
  local right_digit=0
  local borrow=0
  local digit=0
  local result=""
  left="$(normalize_uint "$1")"
  right="$(normalize_uint "$2")"
  if uint_gt "${right}" "${left}"; then
    return 1
  fi
  if [[ "${left}" == "${right}" ]]; then
    printf '0'
    return 0
  fi

  left_index="${#left}"
  right_index="${#right}"
  while [[ "${left_index}" -gt 0 ]]; do
    left_index=$((left_index - 1))
    left_digit="${left:${left_index}:1}"
    right_digit=0
    if [[ "${right_index}" -gt 0 ]]; then
      right_index=$((right_index - 1))
      right_digit="${right:${right_index}:1}"
    fi
    digit=$((left_digit - borrow - right_digit))
    if [[ "${digit}" -lt 0 ]]; then
      digit=$((digit + 10))
      borrow=1
    else
      borrow=0
    fi
    result="${digit}${result}"
  done
  if [[ "${borrow}" -ne 0 || "${right_index}" -ne 0 ]]; then
    return 1
  fi
  normalize_uint "${result}"
}

# Multiply an unsigned decimal string by a small nonnegative integer.
uint_multiply_small() {
  local value
  local factor="$2"
  local value_index
  local digit=0
  local carry=0
  local total=0
  local result=""
  value="$(normalize_uint "$1")"
  value_index="${#value}"
  while [[ "${value_index}" -gt 0 ]]; do
    value_index=$((value_index - 1))
    digit="${value:${value_index}:1}"
    total=$((digit * factor + carry))
    result="$((total % 10))${result}"
    carry=$((total / 10))
  done
  while [[ "${carry}" -gt 0 ]]; do
    result="$((carry % 10))${result}"
    carry=$((carry / 10))
  done
  normalize_uint "${result:-0}"
}

PERF_VALIDATION_ERROR=""
validate_perf_counters() {
  local source="$1"
  local perf_event=""
  local event_count=""
  local event_list="${PERF_EVENTS//,/ }"
  PERF_VALIDATION_ERROR=""

  if [[ ! -s "${source}" ]]; then
    PERF_VALIDATION_ERROR="perf counter output is empty"
    return 1
  fi
  for perf_event in ${event_list}; do
    event_count="$(awk -F';' -v expected="${perf_event}" '
      {
        event = $3
        count = $1
        sub(/^[[:space:]]+/, "", event)
        sub(/[[:space:]]+$/, "", event)
        sub(/^[[:space:]]+/, "", count)
        sub(/[[:space:]]+$/, "", count)
        hybrid_core = "cpu_core/" expected "/"
        hybrid_atom = "cpu_atom/" expected "/"
        if (event == expected || event == hybrid_core ||
            event == hybrid_atom) {
          ++matches
          if (count ~ /^[0-9]+$/) {
            ++numeric_matches
            observed_count = count
          }
        }
      }
      END {
        if (matches < 1 || numeric_matches < 1)
          exit 1
        print observed_count
      }
    ' "${source}")" || {
      PERF_VALIDATION_ERROR="perf event ${perf_event} lacks a numeric counter row"
      return 1
    }
    if ! is_uint "${event_count}"; then
      PERF_VALIDATION_ERROR="perf event ${perf_event} has invalid count"
      return 1
    fi
  done
  return 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --trace)
      need_value "$@"
      TRACE="$2"
      shift 2
      ;;
    --cpu)
      need_value "$@"
      CPU="$2"
      shift 2
      ;;
    --numa-node)
      need_value "$@"
      NUMA_NODE="$2"
      shift 2
      ;;
    --monitor-cpu)
      need_value "$@"
      MONITOR_CPU="$2"
      shift 2
      ;;
    --expect-records)
      need_value "$@"
      EXPECTED_RECORDS="$2"
      shift 2
      ;;
    --expect-bytes)
      need_value "$@"
      EXPECTED_BYTES="$2"
      shift 2
      ;;
    --max-p50-ns)
      need_value "$@"
      MAX_P50_NS="$2"
      shift 2
      ;;
    --max-p99-ns)
      need_value "$@"
      MAX_P99_NS="$2"
      shift 2
      ;;
    --max-p99-9-ns)
      need_value "$@"
      MAX_P999_NS="$2"
      shift 2
      ;;
    --runs)
      need_value "$@"
      RUNS="$2"
      shift 2
      ;;
    --binary)
      need_value "$@"
      BINARY="$2"
      shift 2
      ;;
    --expect-hot-arena-schema)
      need_value "$@"
      EXPECTED_HOT_ARENA_SCHEMA="$2"
      shift 2
      ;;
    --output-dir)
      need_value "$@"
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --sample-every)
      need_value "$@"
      SAMPLE_EVERY="$2"
      shift 2
      ;;
    --warmup-book-messages)
      need_value "$@"
      WARMUP_BOOK_MESSAGES="$2"
      shift 2
      ;;
    --min-samples)
      need_value "$@"
      MIN_SAMPLES="$2"
      shift 2
      ;;
    --planned-bytes)
      need_value "$@"
      PLANNED_BYTES_OVERRIDE="$2"
      shift 2
      ;;
    --reserve-bytes)
      need_value "$@"
      RESERVE_BYTES="$2"
      shift 2
      ;;
    --direct-order-slots)
      need_value "$@"
      DIRECT_ORDER_SLOTS="$2"
      shift 2
      ;;
    --fallback-buckets)
      need_value "$@"
      FALLBACK_BUCKETS="$2"
      shift 2
      ;;
    --price-page-capacity)
      need_value "$@"
      PRICE_PAGE_CAPACITY="$2"
      shift 2
      ;;
    --capacity-profile-name)
      need_value "$@"
      CAPACITY_PROFILE_NAME="$2"
      shift 2
      ;;
    --capacity-evidence-file)
      need_value "$@"
      CAPACITY_EVIDENCE_FILE="$2"
      shift 2
      ;;
    --capacity-evidence-sha256)
      need_value "$@"
      CAPACITY_EVIDENCE_SHA256="$2"
      shift 2
      ;;
    --default-order-capacity)
      need_value "$@"
      DEFAULT_ORDER_CAPACITY="$2"
      shift 2
      ;;
    --price-node-capacity)
      need_value "$@"
      PRICE_NODE_CAPACITY="$2"
      shift 2
      ;;
    --price-leaf-capacity)
      need_value "$@"
      PRICE_LEAF_CAPACITY="$2"
      shift 2
      ;;
    --price-level-capacity)
      need_value "$@"
      PRICE_LEVEL_CAPACITY="$2"
      shift 2
      ;;
    --sample-capacity)
      need_value "$@"
      SAMPLE_CAPACITY="$2"
      shift 2
      ;;
    --correctness-digest)
      DISCOVER_DIGEST=1
      shift
      ;;
    --expect-mutation-digest)
      need_value "$@"
      EXPECTED_MUTATION_DIGEST="$2"
      shift 2
      ;;
    --expect-semantic-mutation-digest)
      need_value "$@"
      EXPECTED_SEMANTIC_MUTATION_DIGEST="$2"
      shift 2
      ;;
    --allow-swap)
      ALLOW_SWAP=1
      shift
      ;;
    --no-perf-stat)
      RUN_PERF_STAT=0
      shift
      ;;
    --hash-trace)
      HASH_TRACE=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ -n "${TRACE}" ]] || die "--trace is required"
[[ -n "${CPU}" ]] || die "--cpu is required"
[[ -n "${NUMA_NODE}" ]] || die "--numa-node is required"
[[ -n "${EXPECTED_RECORDS}" ]] || die "--expect-records is required"
[[ -n "${EXPECTED_BYTES}" ]] || die "--expect-bytes is required"
[[ -n "${MAX_P50_NS}" ]] || die "--max-p50-ns is required"
[[ -n "${MAX_P99_NS}" ]] || die "--max-p99-ns is required"
[[ -n "${MAX_P999_NS}" ]] || die "--max-p99-9-ns is required"
if [[ -n "${EXPECTED_HOT_ARENA_SCHEMA}" &&
      "${EXPECTED_HOT_ARENA_SCHEMA}" != redesign_v1 &&
      "${EXPECTED_HOT_ARENA_SCHEMA}" != branch5_native_v1 ]]; then
  die "--expect-hot-arena-schema must be redesign_v1 or branch5_native_v1"
fi
if [[ "${DRY_RUN}" -eq 1 && -z "${EXPECTED_HOT_ARENA_SCHEMA}" ]]; then
  die "--expect-hot-arena-schema is required with --dry-run"
fi

CAPACITY_IDENTITY_FIELD_COUNT=0
[[ -n "${CAPACITY_PROFILE_NAME}" ]] &&
  CAPACITY_IDENTITY_FIELD_COUNT=$((CAPACITY_IDENTITY_FIELD_COUNT + 1))
[[ -n "${CAPACITY_EVIDENCE_FILE}" ]] &&
  CAPACITY_IDENTITY_FIELD_COUNT=$((CAPACITY_IDENTITY_FIELD_COUNT + 1))
[[ -n "${CAPACITY_EVIDENCE_SHA256}" ]] &&
  CAPACITY_IDENTITY_FIELD_COUNT=$((CAPACITY_IDENTITY_FIELD_COUNT + 1))
if [[ "${CAPACITY_IDENTITY_FIELD_COUNT}" -ne 0 &&
      "${CAPACITY_IDENTITY_FIELD_COUNT}" -ne 3 ]]; then
  die "--capacity-profile-name, --capacity-evidence-file, and --capacity-evidence-sha256 must be supplied together"
fi
if [[ -n "${CAPACITY_EVIDENCE_SHA256}" ]] &&
   ! is_sha256 "${CAPACITY_EVIDENCE_SHA256}"; then
  die "--capacity-evidence-sha256 must be exactly 64 lowercase hex digits"
fi

require_uint "${CPU}" "--cpu"
require_uint "${NUMA_NODE}" "--numa-node"
if [[ -n "${MONITOR_CPU}" ]]; then
  require_uint "${MONITOR_CPU}" "--monitor-cpu"
fi
require_positive_uint "${EXPECTED_RECORDS}" "--expect-records"
require_positive_uint "${EXPECTED_BYTES}" "--expect-bytes"
require_positive_uint "${MAX_P50_NS}" "--max-p50-ns"
require_positive_uint "${MAX_P99_NS}" "--max-p99-ns"
require_positive_uint "${MAX_P999_NS}" "--max-p99-9-ns"
require_positive_uint "${RUNS}" "--runs"
require_positive_uint "${SAMPLE_EVERY}" "--sample-every"
require_uint "${WARMUP_BOOK_MESSAGES}" "--warmup-book-messages"
require_positive_uint "${MIN_SAMPLES}" "--min-samples"
if [[ -n "${PLANNED_BYTES_OVERRIDE}" ]]; then
  require_positive_uint "${PLANNED_BYTES_OVERRIDE}" "--planned-bytes"
fi
require_uint "${RESERVE_BYTES}" "--reserve-bytes"
if [[ -n "${DIRECT_ORDER_SLOTS}" ]]; then
  require_positive_uint "${DIRECT_ORDER_SLOTS}" "--direct-order-slots"
fi
if [[ -n "${FALLBACK_BUCKETS}" ]]; then
  require_positive_uint "${FALLBACK_BUCKETS}" "--fallback-buckets"
fi
if [[ -n "${PRICE_PAGE_CAPACITY}" ]]; then
  require_positive_uint "${PRICE_PAGE_CAPACITY}" "--price-page-capacity"
fi
if [[ -n "${DEFAULT_ORDER_CAPACITY}" ]]; then
  require_positive_uint "${DEFAULT_ORDER_CAPACITY}" \
    "--default-order-capacity"
fi
if [[ -n "${PRICE_NODE_CAPACITY}" ]]; then
  require_positive_uint "${PRICE_NODE_CAPACITY}" "--price-node-capacity"
fi
if [[ -n "${PRICE_LEAF_CAPACITY}" ]]; then
  require_positive_uint "${PRICE_LEAF_CAPACITY}" "--price-leaf-capacity"
fi
if [[ -n "${PRICE_LEVEL_CAPACITY}" ]]; then
  require_positive_uint "${PRICE_LEVEL_CAPACITY}" "--price-level-capacity"
fi
if [[ -n "${SAMPLE_CAPACITY}" ]]; then
  require_positive_uint "${SAMPLE_CAPACITY}" "--sample-capacity"
fi
if [[ -n "${EXPECTED_MUTATION_DIGEST}" ]]; then
  require_uint "${EXPECTED_MUTATION_DIGEST}" "--expect-mutation-digest"
fi
if [[ -n "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" ]]; then
  require_uint "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" \
    "--expect-semantic-mutation-digest"
fi

UINT64_MAX_VALUE=18446744073709551615
for value_and_label in \
  "${EXPECTED_RECORDS}:--expect-records" \
  "${EXPECTED_BYTES}:--expect-bytes" \
  "${MAX_P50_NS}:--max-p50-ns" \
  "${MAX_P99_NS}:--max-p99-ns" \
  "${MAX_P999_NS}:--max-p99-9-ns" \
  "${SAMPLE_EVERY}:--sample-every" \
  "${WARMUP_BOOK_MESSAGES}:--warmup-book-messages" \
  "${MIN_SAMPLES}:--min-samples" \
  "${RESERVE_BYTES}:--reserve-bytes"; do
  value="${value_and_label%%:*}"
  label="${value_and_label#*:}"
  if uint_gt "${value}" "${UINT64_MAX_VALUE}"; then
    die "${label} must fit uint64"
  fi
done
for optional_value_and_label in \
  "${PLANNED_BYTES_OVERRIDE}:--planned-bytes" \
  "${DIRECT_ORDER_SLOTS}:--direct-order-slots" \
  "${FALLBACK_BUCKETS}:--fallback-buckets" \
  "${PRICE_PAGE_CAPACITY}:--price-page-capacity" \
  "${DEFAULT_ORDER_CAPACITY}:--default-order-capacity" \
  "${PRICE_NODE_CAPACITY}:--price-node-capacity" \
  "${PRICE_LEAF_CAPACITY}:--price-leaf-capacity" \
  "${PRICE_LEVEL_CAPACITY}:--price-level-capacity" \
  "${SAMPLE_CAPACITY}:--sample-capacity" \
  "${EXPECTED_MUTATION_DIGEST}:--expect-mutation-digest" \
  "${EXPECTED_SEMANTIC_MUTATION_DIGEST}:--expect-semantic-mutation-digest"; do
  value="${optional_value_and_label%%:*}"
  label="${optional_value_and_label#*:}"
  if [[ -n "${value}" ]] && uint_gt "${value}" "${UINT64_MAX_VALUE}"; then
    die "${label} must fit uint64"
  fi
done
if uint_gt "${CPU}" 2147483647 || uint_gt "${NUMA_NODE}" 2147483647; then
  die "CPU and NUMA-node identifiers must fit signed 32-bit"
fi
if [[ -n "${MONITOR_CPU}" ]] && uint_gt "${MONITOR_CPU}" 2147483647; then
  die "CPU and NUMA-node identifiers must fit signed 32-bit"
fi

CPU="$(normalize_uint "${CPU}")"
NUMA_NODE="$(normalize_uint "${NUMA_NODE}")"
if [[ -z "${MONITOR_CPU}" ]]; then
  if [[ "${CPU}" == 0 ]]; then
    MONITOR_CPU=1
  else
    MONITOR_CPU=0
  fi
else
  MONITOR_CPU="$(normalize_uint "${MONITOR_CPU}")"
fi
EXPECTED_RECORDS="$(normalize_uint "${EXPECTED_RECORDS}")"
EXPECTED_BYTES="$(normalize_uint "${EXPECTED_BYTES}")"
MAX_P50_NS="$(normalize_uint "${MAX_P50_NS}")"
MAX_P99_NS="$(normalize_uint "${MAX_P99_NS}")"
MAX_P999_NS="$(normalize_uint "${MAX_P999_NS}")"
RUNS="$(normalize_uint "${RUNS}")"
SAMPLE_EVERY="$(normalize_uint "${SAMPLE_EVERY}")"
WARMUP_BOOK_MESSAGES="$(normalize_uint "${WARMUP_BOOK_MESSAGES}")"
MIN_SAMPLES="$(normalize_uint "${MIN_SAMPLES}")"
if [[ -n "${PLANNED_BYTES_OVERRIDE}" ]]; then
  PLANNED_BYTES_OVERRIDE="$(normalize_uint "${PLANNED_BYTES_OVERRIDE}")"
fi
RESERVE_BYTES="$(normalize_uint "${RESERVE_BYTES}")"
if [[ -n "${DIRECT_ORDER_SLOTS}" ]]; then
  DIRECT_ORDER_SLOTS="$(normalize_uint "${DIRECT_ORDER_SLOTS}")"
fi
if [[ -n "${FALLBACK_BUCKETS}" ]]; then
  FALLBACK_BUCKETS="$(normalize_uint "${FALLBACK_BUCKETS}")"
fi
if [[ -n "${PRICE_PAGE_CAPACITY}" ]]; then
  PRICE_PAGE_CAPACITY="$(normalize_uint "${PRICE_PAGE_CAPACITY}")"
fi
if [[ -n "${DEFAULT_ORDER_CAPACITY}" ]]; then
  DEFAULT_ORDER_CAPACITY="$(normalize_uint "${DEFAULT_ORDER_CAPACITY}")"
fi
if [[ -n "${PRICE_NODE_CAPACITY}" ]]; then
  PRICE_NODE_CAPACITY="$(normalize_uint "${PRICE_NODE_CAPACITY}")"
fi
if [[ -n "${PRICE_LEAF_CAPACITY}" ]]; then
  PRICE_LEAF_CAPACITY="$(normalize_uint "${PRICE_LEAF_CAPACITY}")"
fi
if [[ -n "${PRICE_LEVEL_CAPACITY}" ]]; then
  PRICE_LEVEL_CAPACITY="$(normalize_uint "${PRICE_LEVEL_CAPACITY}")"
fi
if [[ -n "${SAMPLE_CAPACITY}" ]]; then
  SAMPLE_CAPACITY="$(normalize_uint "${SAMPLE_CAPACITY}")"
fi
if [[ -n "${EXPECTED_MUTATION_DIGEST}" ]]; then
  EXPECTED_MUTATION_DIGEST="$(normalize_uint "${EXPECTED_MUTATION_DIGEST}")"
fi
if [[ -n "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" ]]; then
  EXPECTED_SEMANTIC_MUTATION_DIGEST="$(normalize_uint \
    "${EXPECTED_SEMANTIC_MUTATION_DIGEST}")"
fi

if uint_gt 5 "${RUNS}"; then
  die "--runs must be at least 5"
fi
if uint_gt "${RUNS}" 100; then
  die "--runs must not exceed 100"
fi
if uint_gt "${MAX_P50_NS}" "${MAX_P99_NS}" ||
   uint_gt "${MAX_P99_NS}" "${MAX_P999_NS}"; then
  die "latency ceilings must be monotonic: p50 <= p99 <= p99.9"
fi
if [[ "${MONITOR_CPU}" == "${CPU}" ]]; then
  die "--monitor-cpu must differ from the candidate --cpu"
fi
if [[ -n "${FALLBACK_BUCKETS}" ]]; then
  if uint_gt "${FALLBACK_BUCKETS}" 4294967295 ||
     [[ $((FALLBACK_BUCKETS & (FALLBACK_BUCKETS - 1))) -ne 0 ]]; then
    die "--fallback-buckets must be a uint32 power of two"
  fi
fi
if [[ -n "${PRICE_PAGE_CAPACITY}" ]] &&
   uint_ge "${PRICE_PAGE_CAPACITY}" 4294967295; then
  die "--price-page-capacity must be below UINT32_MAX"
fi
if [[ -n "${SAMPLE_CAPACITY}" ]] &&
   ! uint_ge "${SAMPLE_CAPACITY}" "${MIN_SAMPLES}"; then
  die "--sample-capacity must be at least --min-samples"
fi
if [[ "${DISCOVER_DIGEST}" -eq 1 &&
      ( -n "${EXPECTED_MUTATION_DIGEST}" ||
        -n "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" ) ]]; then
  die "--correctness-digest and expected digest options are mutually exclusive"
fi
if [[ -n "${PLANNED_BYTES_OVERRIDE}" ]] &&
   uint_gt "${PLANNED_BYTES_OVERRIDE}" 9223372036854775807; then
  die "planned byte override must fit signed 64-bit arithmetic"
fi
if uint_gt "${RESERVE_BYTES}" 9223372036854775807; then
  die "reserve byte value must fit signed 64-bit arithmetic"
fi

if [[ -z "${OUTPUT_DIR}" ]]; then
  run_stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  OUTPUT_DIR="${ROOT_DIR}/benchmark-results/order-book-${run_stamp}-$$"
fi

build_storage_options() {
  STORAGE_OPTIONS=()
  if [[ -n "${CAPACITY_PROFILE_NAME}" ]]; then
    STORAGE_OPTIONS+=(
      "--capacity-profile-name=${CAPACITY_PROFILE_NAME}"
      "--capacity-evidence-file=${CAPACITY_EVIDENCE_FILE}"
      "--capacity-evidence-sha256=${CAPACITY_EVIDENCE_SHA256}"
    )
  fi
  if [[ -n "${DIRECT_ORDER_SLOTS}" ]]; then
    STORAGE_OPTIONS+=("--direct-order-slots=${DIRECT_ORDER_SLOTS}")
  fi
  if [[ -n "${FALLBACK_BUCKETS}" ]]; then
    STORAGE_OPTIONS+=("--fallback-buckets=${FALLBACK_BUCKETS}")
  fi
  if [[ -n "${PRICE_PAGE_CAPACITY}" ]]; then
    STORAGE_OPTIONS+=("--price-page-capacity=${PRICE_PAGE_CAPACITY}")
  fi
  if [[ -n "${DEFAULT_ORDER_CAPACITY}" ]]; then
    STORAGE_OPTIONS+=("--default-order-capacity=${DEFAULT_ORDER_CAPACITY}")
  fi
  if [[ -n "${PRICE_NODE_CAPACITY}" ]]; then
    STORAGE_OPTIONS+=("--price-node-capacity=${PRICE_NODE_CAPACITY}")
  fi
  if [[ -n "${PRICE_LEAF_CAPACITY}" ]]; then
    STORAGE_OPTIONS+=("--price-leaf-capacity=${PRICE_LEAF_CAPACITY}")
  fi
  if [[ -n "${PRICE_LEVEL_CAPACITY}" ]]; then
    STORAGE_OPTIONS+=("--price-level-capacity=${PRICE_LEVEL_CAPACITY}")
  fi
  if [[ -n "${SAMPLE_CAPACITY}" ]]; then
    STORAGE_OPTIONS+=("--sample-capacity=${SAMPLE_CAPACITY}")
  fi
}

build_base_command() {
  BASE_COMMAND=(
    "${NUMACTL_BIN}"
    "--physcpubind=${CPU}"
    "--membind=${NUMA_NODE}"
    "${BINARY}"
    "${TRACE}"
    --prefault
  )
  if [[ "${#STORAGE_OPTIONS[@]}" -gt 0 ]]; then
    BASE_COMMAND+=("${STORAGE_OPTIONS[@]}")
  fi
  BASE_COMMAND+=(
    "--sample-every=${SAMPLE_EVERY}"
    "--warmup-book-messages=${WARMUP_BOOK_MESSAGES}"
    "--min-samples=${MIN_SAMPLES}"
    "--expect-records=${EXPECTED_RECORDS}"
    "--expect-bytes=${EXPECTED_BYTES}"
    --require-zero-post-warmup-faults
  )
}

build_plan_command() {
  PLAN_COMMAND=(
    "${NUMACTL_BIN}"
    "--physcpubind=${CPU}"
    "--membind=${NUMA_NODE}"
    "${BINARY}"
    "${TRACE}"
    --prefault
  )
  if [[ "${#STORAGE_OPTIONS[@]}" -gt 0 ]]; then
    PLAN_COMMAND+=("${STORAGE_OPTIONS[@]}")
  fi
  PLAN_COMMAND+=("--min-samples=${MIN_SAMPLES}" --storage-plan-only)
}

print_command() {
  printf '  '
  printf '%q ' "$@"
  printf '\n'
}

write_command_file() {
  local destination="$1"
  shift
  printf '%q ' "$@" >"${destination}"
  printf '\n' >>"${destination}"
}

write_argv_file() {
  local destination="$1"
  shift
  printf '%s\0' "$@" >"${destination}"
}

binary_matches_fresh_source_build() {
  local supplied_path="$1"
  local supplied_sha256="$2"
  local fresh_path="$3"
  local fresh_sha256="$4"
  local observed_supplied_sha256=""
  local observed_fresh_sha256=""

  [[ "${supplied_sha256}" =~ ^[0-9a-f]{64}$ &&
     "${fresh_sha256}" =~ ^[0-9a-f]{64}$ &&
     "${supplied_sha256}" == "${fresh_sha256}" &&
     -f "${supplied_path}" && -f "${fresh_path}" ]] || return 1
  observed_supplied_sha256="$(sha256sum -- "${supplied_path}" |
    awk '{ print $1 }')" || return 1
  observed_fresh_sha256="$(sha256sum -- "${fresh_path}" |
    awk '{ print $1 }')" || return 1
  [[ "${observed_supplied_sha256}" == "${supplied_sha256}" &&
     "${observed_fresh_sha256}" == "${fresh_sha256}" ]] || return 1
  cmp -s -- "${supplied_path}" "${fresh_path}"
}

build_storage_options
build_base_command
build_plan_command

if [[ "${DRY_RUN}" -eq 1 ]]; then
  if ! configure_hot_arena_schema "${EXPECTED_HOT_ARENA_SCHEMA}"; then
    die "unsupported expected hot-arena schema: ${EXPECTED_HOT_ARENA_SCHEMA}"
  fi
  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      if [[ -n "${DEFAULT_ORDER_CAPACITY}" ||
            -n "${PRICE_NODE_CAPACITY}" ||
            -n "${PRICE_LEAF_CAPACITY}" ||
            -n "${PRICE_LEVEL_CAPACITY}" ]]; then
        die "branch5 capacity options are invalid for redesign_v1"
      fi
      ;;
    branch5_native_ranges_v1)
      if [[ -n "${DIRECT_ORDER_SLOTS}" ||
            -n "${FALLBACK_BUCKETS}" ||
            -n "${PRICE_PAGE_CAPACITY}" ||
            -n "${CAPACITY_PROFILE_NAME}" ]]; then
        die "redesign capacity options are invalid for branch5_native_v1"
      fi
      if ! branch5_runtime_capacity_request_is_canonical \
           "${DEFAULT_ORDER_CAPACITY}" "${PRICE_NODE_CAPACITY}" \
           "${PRICE_LEAF_CAPACITY}" "${PRICE_LEVEL_CAPACITY}"; then
        die "branch5 capacity flags must be omitted together or all equal the canonical pinned values"
      fi
      if [[ -z "${PLANNED_BYTES_OVERRIDE}" ]]; then
        die "branch5_native_v1 requires --planned-bytes because per-book bytes resolve only after the R/S prelude"
      fi
      ;;
  esac
  echo "dry_run=1 linux_preflight=skipped filesystem_checks=skipped"
  echo "expected_hot_arena_schema=${EXPECTED_HOT_ARENA_SCHEMA} schema_probe=skipped"
  echo "output_dir=${OUTPUT_DIR}"
  if [[ -n "${PLANNED_BYTES_OVERRIDE}" ]]; then
    echo "planned_bytes_admission_override=${PLANNED_BYTES_OVERRIDE} reserve_bytes=${RESERVE_BYTES}"
  else
    echo "planned_bytes=derived_from_storage_plan reserve_bytes=${RESERVE_BYTES}"
  fi
  echo "candidate_cpu=${CPU} monitor_cpu=${MONITOR_CPU} numa_node=${NUMA_NODE}"
  echo "storage_plan command="
  print_command "${PLAN_COMMAND[@]}"
  for ((run = 1; run <= RUNS; ++run)); do
    printf -v run_id '%03d' "${run}"
    command=(
      "${BASE_COMMAND[@]}"
      "--max-p50-ns=${MAX_P50_NS}"
      "--max-p99-ns=${MAX_P99_NS}"
      "--max-p99-9-ns=${MAX_P999_NS}"
    )
    if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
      command+=(
        "--native-range-manifest=${OUTPUT_DIR}/latency-${run_id}.native-ranges.txt"
      )
    fi
    command+=(
      "--start-gate-file=${OUTPUT_DIR}/latency-${run_id}.start-gate"
      "--start-gate-timeout-ms=600000"
    )
    printf 'latency_run=%d command=\n' "${run}"
    print_command "${command[@]}"
  done
  if [[ "${DISCOVER_DIGEST}" -eq 1 ]]; then
    echo "correctness_discovery command="
    command=("${BASE_COMMAND[@]}" --mutation-digest)
    if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
      command+=(
        "--native-range-manifest=${OUTPUT_DIR}/correctness-discovery.native-ranges.txt"
      )
    fi
    command+=(
      "--start-gate-file=${OUTPUT_DIR}/correctness-discovery.start-gate"
      --start-gate-timeout-ms=600000
    )
    print_command "${command[@]}"
    echo "correctness_verification command="
    command=(
      "${BASE_COMMAND[@]}"
      '--expect-mutation-digest=<digest-from-discovery>' \
      '--expect-semantic-mutation-digest=<semantic-digest-from-discovery>' \
    )
    if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
      command+=(
        "--native-range-manifest=${OUTPUT_DIR}/correctness-verification.native-ranges.txt"
      )
    fi
    command+=(
      "--start-gate-file=${OUTPUT_DIR}/correctness-verification.start-gate"
      --start-gate-timeout-ms=600000
    )
    print_command "${command[@]}"
  elif [[ -n "${EXPECTED_MUTATION_DIGEST}" ||
          -n "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" ]]; then
    echo "correctness_verification command="
    command=("${BASE_COMMAND[@]}")
    if [[ -n "${EXPECTED_MUTATION_DIGEST}" ]]; then
      command+=("--expect-mutation-digest=${EXPECTED_MUTATION_DIGEST}")
    fi
    if [[ -n "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" ]]; then
      command+=("--expect-semantic-mutation-digest=${EXPECTED_SEMANTIC_MUTATION_DIGEST}")
    fi
    if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
      command+=(
        "--native-range-manifest=${OUTPUT_DIR}/correctness-verification.native-ranges.txt"
      )
    fi
    command+=(
      "--start-gate-file=${OUTPUT_DIR}/correctness-verification.start-gate"
      --start-gate-timeout-ms=600000
    )
    print_command "${command[@]}"
  fi
  if [[ "${RUN_PERF_STAT}" -eq 1 ]]; then
    echo "hardware_counters command="
    command=(
      "${PERF_BIN}" stat -x ';'
      -o "${OUTPUT_DIR}/hardware-counters.perf-stat.csv"
      -e "${PERF_EVENTS}"
      --
      "${BASE_COMMAND[@]}"
    )
    if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
      command+=(
        "--native-range-manifest=${OUTPUT_DIR}/hardware-counters.native-ranges.txt"
      )
    fi
    print_command "${command[@]}"
  else
    echo "hardware_counters=explicitly_skipped"
  fi
  exit 0
fi

case "$(uname -m)" in
  x86_64|amd64)
    ;;
  *)
    die "live acceptance requires x86_64 RDTSCP; use --dry-run on this host"
    ;;
esac

[[ -x "${BINARY}" ]] || die "replay binary is not executable: ${BINARY}"
[[ -r "${TRACE}" && -f "${TRACE}" ]] || die "trace is not a readable file: ${TRACE}"
if ! command -v numactl >/dev/null 2>&1; then
  die "numactl is required for live acceptance"
fi
NUMACTL_BIN="$(command -v numactl)"
if ! command -v git >/dev/null 2>&1; then
  die "git is required to record worktree provenance"
fi

if [[ -e "${OUTPUT_DIR}" || -L "${OUTPUT_DIR}" ]]; then
  die "output path already exists; refusing to reuse it: ${OUTPUT_DIR}"
fi
mkdir -p "$(dirname "${OUTPUT_DIR}")"
mkdir "${OUTPUT_DIR}"

SUMMARY_FILE="${OUTPUT_DIR}/summary.tsv"
PREFLIGHT_FILE="${OUTPUT_DIR}/preflight.txt"
printf 'kind\tid\tstatus\texit_code\tp50_ns\tp99_ns\tp99_9_ns\tdetail\tstdout\tstderr\n' >"${SUMMARY_FILE}"
ARTIFACTS_INITIALIZED=1

preflight_fail() {
  local message="$1"
  echo "preflight_failure=${message}" | tee -a "${PREFLIGHT_FILE}" >&2
  printf 'overall\t-\tFAIL\t-\t-\t-\t-\t%s\t-\t-\n' "${message}" >>"${SUMMARY_FILE}"
  echo "artifacts=${OUTPUT_DIR}" >&2
  exit 1
}

if ! command -v sha256sum >/dev/null 2>&1; then
  preflight_fail "sha256sum is required for live provenance"
fi
if ! command -v readlink >/dev/null 2>&1; then
  preflight_fail "readlink is required for live provenance"
fi
if ! command -v cmp >/dev/null 2>&1; then
  preflight_fail "cmp is required for deterministic storage-plan verification"
fi

BINARY_REQUESTED="${BINARY}"
TRACE_REQUESTED="${TRACE}"
CAPACITY_EVIDENCE_FILE_REQUESTED="${CAPACITY_EVIDENCE_FILE}"
BINARY="$(readlink -f -- "${BINARY}")" ||
  preflight_fail "cannot resolve replay binary"
TRACE="$(readlink -f -- "${TRACE}")" ||
  preflight_fail "cannot resolve trace"
if [[ -n "${CAPACITY_EVIDENCE_FILE}" ]]; then
  CAPACITY_EVIDENCE_FILE="$(readlink -f -- "${CAPACITY_EVIDENCE_FILE}")" ||
    preflight_fail "cannot resolve capacity evidence manifest"
fi
HARNESS_SOURCE="$(readlink -f -- "${BASH_SOURCE[0]}")" ||
  preflight_fail "cannot resolve acceptance harness"
[[ -x "${BINARY}" && -f "${BINARY}" ]] ||
  preflight_fail "resolved replay binary is not an executable file"
[[ -r "${TRACE}" && -f "${TRACE}" ]] ||
  preflight_fail "resolved trace is not a readable file"
if [[ -n "${CAPACITY_EVIDENCE_FILE}" ]]; then
  [[ -r "${CAPACITY_EVIDENCE_FILE}" &&
     -f "${CAPACITY_EVIDENCE_FILE}" ]] ||
    preflight_fail "resolved capacity evidence manifest is not a readable file"
fi
[[ -r "${HARNESS_SOURCE}" && -f "${HARNESS_SOURCE}" ]] ||
  preflight_fail "resolved acceptance harness is not readable"

# Rebuild commands with canonical live paths. The replay process itself hashes
# and parses this same file; shell-side capture is retention and stability
# evidence, not the authority for its capacity values.
build_storage_options
build_base_command
build_plan_command

PROVENANCE_DIR="${OUTPUT_DIR}/provenance"
BUILD_PROVENANCE_DIR="${PROVENANCE_DIR}/build"
mkdir -p "${BUILD_PROVENANCE_DIR}"

FILE_STATE_SHA256=""
FILE_STATE_STAT=""
FILE_STATE_SIZE=""
FILE_STATE_ERROR=""
capture_file_state() {
  local path="$1"
  local destination="$2"
  local stat_before=""
  local stat_after=""
  local digest=""

  FILE_STATE_SHA256=""
  FILE_STATE_STAT=""
  FILE_STATE_SIZE=""
  FILE_STATE_ERROR=""
  if ! stat_before="$(stat -Lc \
      'device=%d inode=%i mode=%f size=%s mtime=%y ctime=%z' -- "${path}")"; then
    FILE_STATE_ERROR="cannot stat ${path} before hashing"
    return 1
  fi
  if ! digest="$(sha256sum -- "${path}" | awk '{ print $1 }')"; then
    FILE_STATE_ERROR="cannot hash ${path}"
    return 1
  fi
  if [[ ! "${digest}" =~ ^[0-9a-f]{64}$ ]]; then
    FILE_STATE_ERROR="invalid SHA-256 for ${path}"
    return 1
  fi
  if ! stat_after="$(stat -Lc \
      'device=%d inode=%i mode=%f size=%s mtime=%y ctime=%z' -- "${path}")"; then
    FILE_STATE_ERROR="cannot stat ${path} after hashing"
    return 1
  fi

  {
    printf 'path='; printf '%q\n' "${path}"
    echo "sha256=${digest}"
    echo "stat_before_hash=${stat_before}"
    echo "stat_after_hash=${stat_after}"
    if [[ "${stat_before}" == "${stat_after}" ]]; then
      echo "stable_during_capture=1"
    else
      echo "stable_during_capture=0"
    fi
  } >"${destination}"

  if [[ "${stat_before}" != "${stat_after}" ]]; then
    FILE_STATE_ERROR="${path} changed while its provenance was captured"
    return 1
  fi
  FILE_STATE_SHA256="${digest}"
  FILE_STATE_STAT="${stat_after}"
  FILE_STATE_SIZE="$(stat -Lc %s -- "${path}")" || {
    FILE_STATE_ERROR="cannot read ${path} size"
    return 1
  }
  return 0
}

CAPACITY_EVIDENCE_POLICY=not_applicable_branch5_or_builtin
CAPACITY_EVIDENCE_SHA256_BEFORE=not_applicable
CAPACITY_EVIDENCE_STAT_BEFORE=not_applicable
CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE=not_applicable
CAPACITY_EVIDENCE_ARCHIVE_STAT_BEFORE=not_applicable
CAPACITY_EVIDENCE_ARCHIVE=not_applicable
if [[ -n "${CAPACITY_EVIDENCE_FILE}" ]]; then
  CAPACITY_EVIDENCE_PROVENANCE_SOURCE="${CAPACITY_EVIDENCE_FILE}"
  CAPACITY_EVIDENCE_EXPECTED_SHA256="${CAPACITY_EVIDENCE_SHA256}"
  CAPACITY_EVIDENCE_POLICY=canonical_manifest_v1
  if ! capture_file_state "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" \
       "${PROVENANCE_DIR}/capacity-evidence-source-before.state"; then
    preflight_fail "${FILE_STATE_ERROR}"
  fi
  CAPACITY_EVIDENCE_SHA256_BEFORE="${FILE_STATE_SHA256}"
  CAPACITY_EVIDENCE_STAT_BEFORE="${FILE_STATE_STAT}"
  if [[ "${CAPACITY_EVIDENCE_SHA256_BEFORE}" != \
        "${CAPACITY_EVIDENCE_EXPECTED_SHA256}" ]]; then
    preflight_fail "capacity evidence manifest differs from --capacity-evidence-sha256"
  fi
  CAPACITY_EVIDENCE_ARCHIVE="${PROVENANCE_DIR}/capacity-evidence-manifest.txt"
  if ! cp --preserve=mode,timestamps -- \
       "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" \
       "${CAPACITY_EVIDENCE_ARCHIVE}"; then
    preflight_fail "cannot archive capacity evidence manifest"
  fi
  if ! capture_file_state "${CAPACITY_EVIDENCE_ARCHIVE}" \
       "${PROVENANCE_DIR}/capacity-evidence-archive-before.state"; then
    preflight_fail "${FILE_STATE_ERROR}"
  fi
  CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
  CAPACITY_EVIDENCE_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
  if [[ "${CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE}" != \
        "${CAPACITY_EVIDENCE_SHA256_BEFORE}" ]]; then
    preflight_fail "archived capacity evidence manifest differs from source"
  fi
fi

if ! capture_file_state "${BINARY}" \
     "${PROVENANCE_DIR}/binary-supplied.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
SUPPLIED_BINARY_SHA256="${FILE_STATE_SHA256}"
SUPPLIED_BINARY_STAT="${FILE_STATE_STAT}"
SUPPLIED_BINARY_SIZE_BYTES="${FILE_STATE_SIZE}"
BINARY_SHA256_BEFORE="${SUPPLIED_BINARY_SHA256}"
BINARY_STAT_BEFORE="${SUPPLIED_BINARY_STAT}"
BINARY_SIZE_BYTES="${SUPPLIED_BINARY_SIZE_BYTES}"
if ! capture_file_state "${TRACE}" \
     "${PROVENANCE_DIR}/trace-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
TRACE_SHA256_BEFORE="${FILE_STATE_SHA256}"
TRACE_STAT_BEFORE="${FILE_STATE_STAT}"
TRACE_SIZE_BYTES="${FILE_STATE_SIZE}"
if ! capture_file_state "${HARNESS_SOURCE}" \
     "${PROVENANCE_DIR}/harness-source-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
HARNESS_SHA256_BEFORE="${FILE_STATE_SHA256}"
HARNESS_STAT_BEFORE="${FILE_STATE_STAT}"

if [[ "$(normalize_uint "${TRACE_SIZE_BYTES}")" != "${EXPECTED_BYTES}" ]]; then
  preflight_fail "trace filesystem size differs from --expect-bytes"
fi
BINARY_ARCHIVE="${PROVENANCE_DIR}/tested-binary"
HARNESS_ARCHIVE="${PROVENANCE_DIR}/run_order_book_acceptance.sh"
if ! cp --preserve=mode,timestamps -- "${BINARY}" "${BINARY_ARCHIVE}"; then
  preflight_fail "cannot archive tested replay binary"
fi
if ! cp --preserve=mode,timestamps -- "${HARNESS_SOURCE}" "${HARNESS_ARCHIVE}"; then
  preflight_fail "cannot archive acceptance harness"
fi
if ! capture_file_state "${BINARY_ARCHIVE}" \
     "${PROVENANCE_DIR}/binary-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
BINARY_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
BINARY_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
if [[ "${BINARY_ARCHIVE_SHA256_BEFORE}" != "${BINARY_SHA256_BEFORE}" ]]; then
  preflight_fail "archived replay binary differs from tested binary"
fi
if ! capture_file_state "${HARNESS_ARCHIVE}" \
     "${PROVENANCE_DIR}/harness-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
HARNESS_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
HARNESS_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
if [[ "${HARNESS_ARCHIVE_SHA256_BEFORE}" != "${HARNESS_SHA256_BEFORE}" ]]; then
  preflight_fail "archived acceptance harness differs from running harness"
fi

HOT_PATH_VERIFIER_SOURCE="${ROOT_DIR}/scripts/verify_order_book_hot_path.sh"
[[ -x "${HOT_PATH_VERIFIER_SOURCE}" && -f "${HOT_PATH_VERIFIER_SOURCE}" ]] ||
  preflight_fail "hot-path verifier is missing or not executable"
if ! capture_file_state "${HOT_PATH_VERIFIER_SOURCE}" \
     "${PROVENANCE_DIR}/hot-path-verifier-source-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
HOT_PATH_VERIFIER_SHA256_BEFORE="${FILE_STATE_SHA256}"
HOT_PATH_VERIFIER_STAT_BEFORE="${FILE_STATE_STAT}"
HOT_PATH_VERIFIER_ARCHIVE="${PROVENANCE_DIR}/verify_order_book_hot_path.sh"
if ! cp --preserve=mode,timestamps -- "${HOT_PATH_VERIFIER_SOURCE}" \
     "${HOT_PATH_VERIFIER_ARCHIVE}"; then
  preflight_fail "cannot archive hot-path verifier"
fi
if ! capture_file_state "${HOT_PATH_VERIFIER_ARCHIVE}" \
     "${PROVENANCE_DIR}/hot-path-verifier-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
HOT_PATH_VERIFIER_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
HOT_PATH_VERIFIER_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
if [[ "${HOT_PATH_VERIFIER_ARCHIVE_SHA256_BEFORE}" != \
      "${HOT_PATH_VERIFIER_SHA256_BEFORE}" ]]; then
  preflight_fail "archived hot-path verifier differs from source verifier"
fi
HOT_PATH_VERIFIER_COMMAND_FILE="${PROVENANCE_DIR}/hot-path-verifier.command.txt"
HOT_PATH_VERIFIER_STDOUT="${PROVENANCE_DIR}/hot-path-verifier.stdout.log"
HOT_PATH_VERIFIER_STDERR="${PROVENANCE_DIR}/hot-path-verifier.stderr.log"
HOT_PATH_VERIFIER_REPORT="${PROVENANCE_DIR}/hot-path-disassembly.txt"
HOT_ARENA_SCHEMA_PROBE_STDOUT="${PROVENANCE_DIR}/hot-arena-schema-probe.stdout.log"
HOT_ARENA_SCHEMA_PROBE_STDERR="${PROVENANCE_DIR}/hot-arena-schema-probe.stderr.log"
write_command_file "${PROVENANCE_DIR}/hot-arena-schema-probe.command.txt" \
  "${PLAN_COMMAND[@]}"
if ! "${PLAN_COMMAND[@]}" >"${HOT_ARENA_SCHEMA_PROBE_STDOUT}" \
     2>"${HOT_ARENA_SCHEMA_PROBE_STDERR}"; then
  preflight_fail "hot-arena schema probe failed"
fi
PROBED_HOT_ARENA_SCHEMA="$(awk '
  $1 == "itch_book_replay_storage_plan" {
    ++lines
    for (field_number = 2; field_number <= NF; ++field_number) {
      if ($field_number ~ /^hot_arena_schema=/) {
        value = substr($field_number, length("hot_arena_schema=") + 1)
        ++matches
      }
    }
  }
  END {
    if (lines != 1 || matches != 1 || value == "")
      exit 1
    print value
  }
' "${HOT_ARENA_SCHEMA_PROBE_STDOUT}")" ||
  preflight_fail "hot-arena schema probe output is missing or ambiguous"
if ! configure_hot_arena_schema "${PROBED_HOT_ARENA_SCHEMA}"; then
  preflight_fail "unsupported hot-arena schema: ${PROBED_HOT_ARENA_SCHEMA}"
fi
if ! hot_arena_schema_matches_expectation \
     "${PROBED_HOT_ARENA_SCHEMA}" "${EXPECTED_HOT_ARENA_SCHEMA}"; then
  preflight_fail "binary hot-arena schema ${PROBED_HOT_ARENA_SCHEMA} differs from --expect-hot-arena-schema=${EXPECTED_HOT_ARENA_SCHEMA}"
fi
if [[ "${PROBED_HOT_ARENA_SCHEMA}" == branch5_native_v1 ]]; then
  if ! branch5_runtime_capacity_request_is_canonical \
       "${DEFAULT_ORDER_CAPACITY}" "${PRICE_NODE_CAPACITY}" \
       "${PRICE_LEAF_CAPACITY}" "${PRICE_LEVEL_CAPACITY}"; then
    preflight_fail "branch5 capacity flags must be omitted together or all equal the canonical pinned values"
  fi
  if [[ -n "${DEFAULT_ORDER_CAPACITY}" ]]; then
    BRANCH5_RUNTIME_CAPACITY_MODE=canonical_explicit_v1
  else
    BRANCH5_RUNTIME_CAPACITY_MODE=binary_defaults_v1
  fi
  if [[ "${TRACE_SHA256_BEFORE}" == "${BRANCH5_PINNED_TRACE_SHA256}" ]]; then
    BRANCH5_CAPACITY_POLICY=pinned_trace_canonical_v1
    BRANCH5_CAPACITY_PROFILE_NAME="${BRANCH5_PINNED_CAPACITY_PROFILE_NAME}"
    BRANCH5_CAPACITY_EVIDENCE_POLICY=reviewed_manifest_sha256_v1
    if [[ "${BRANCH5_RUNTIME_CAPACITY_MODE}" != canonical_explicit_v1 ]]; then
      preflight_fail "pinned branch5 trace requires all four canonical capacity flags explicitly"
    fi
    if [[ "${PLANNED_BYTES_OVERRIDE}" != \
          "${BRANCH5_PINNED_PLANNED_BYTES}" ]]; then
      preflight_fail "pinned branch5 trace requires --planned-bytes=${BRANCH5_PINNED_PLANNED_BYTES}"
    fi
    [[ "${RESERVE_BYTES}" == "${BRANCH5_PINNED_RESERVE_BYTES}" ]] ||
      preflight_fail "pinned branch5 trace requires --reserve-bytes=${BRANCH5_PINNED_RESERVE_BYTES}"
    [[ "${SAMPLE_EVERY}" == "${BRANCH5_PINNED_SAMPLE_EVERY}" ]] ||
      preflight_fail "pinned branch5 trace requires --sample-every=${BRANCH5_PINNED_SAMPLE_EVERY}"
    [[ "${SAMPLE_CAPACITY}" == "${BRANCH5_PINNED_SAMPLE_CAPACITY}" ]] ||
      preflight_fail "pinned branch5 trace requires --sample-capacity=${BRANCH5_PINNED_SAMPLE_CAPACITY}"
    [[ -z "${CAPACITY_EVIDENCE_FILE}" ]] ||
      preflight_fail "branch5 native acceptance does not accept a redesign capacity manifest"
    CAPACITY_EVIDENCE_PROVENANCE_SOURCE="$(
      readlink -f -- \
        "${ROOT_DIR}/${BRANCH5_PINNED_CAPACITY_EVIDENCE_RELATIVE}"
    )" || preflight_fail "cannot resolve reviewed branch5 capacity evidence"
    [[ -r "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" &&
       -f "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" ]] ||
      preflight_fail "reviewed branch5 capacity evidence is not a readable file"
    CAPACITY_EVIDENCE_EXPECTED_SHA256="${BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256}"
    if ! capture_file_state "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" \
         "${PROVENANCE_DIR}/capacity-evidence-source-before.state"; then
      preflight_fail "${FILE_STATE_ERROR}"
    fi
    CAPACITY_EVIDENCE_SHA256_BEFORE="${FILE_STATE_SHA256}"
    CAPACITY_EVIDENCE_STAT_BEFORE="${FILE_STATE_STAT}"
    [[ "${CAPACITY_EVIDENCE_SHA256_BEFORE}" == \
       "${CAPACITY_EVIDENCE_EXPECTED_SHA256}" ]] ||
      preflight_fail "reviewed branch5 capacity evidence hash differs"
    CAPACITY_EVIDENCE_ARCHIVE="${PROVENANCE_DIR}/capacity-evidence-manifest.txt"
    if ! cp --preserve=mode,timestamps -- \
         "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" \
         "${CAPACITY_EVIDENCE_ARCHIVE}"; then
      preflight_fail "cannot archive reviewed branch5 capacity evidence"
    fi
    if ! capture_file_state "${CAPACITY_EVIDENCE_ARCHIVE}" \
         "${PROVENANCE_DIR}/capacity-evidence-archive-before.state"; then
      preflight_fail "${FILE_STATE_ERROR}"
    fi
    CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
    CAPACITY_EVIDENCE_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
    [[ "${CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE}" == \
       "${CAPACITY_EVIDENCE_EXPECTED_SHA256}" ]] ||
      preflight_fail "archived branch5 capacity evidence differs from reviewed source"
  else
    BRANCH5_CAPACITY_POLICY=default_binary_unbound_trace_v1
    BRANCH5_CAPACITY_PROFILE_NAME=branch5-default-binary-v1
    BRANCH5_CAPACITY_EVIDENCE_POLICY=unbound_trace_v1
  fi
fi
write_command_file "${HOT_PATH_VERIFIER_COMMAND_FILE}" \
  "${HOT_PATH_VERIFIER_ARCHIVE}" --schema "${PROBED_HOT_ARENA_SCHEMA}" \
  "${BINARY_ARCHIVE}" \
  "${HOT_PATH_VERIFIER_REPORT}"
if ! "${HOT_PATH_VERIFIER_ARCHIVE}" --schema \
     "${PROBED_HOT_ARENA_SCHEMA}" "${BINARY_ARCHIVE}" \
     "${HOT_PATH_VERIFIER_REPORT}" >"${HOT_PATH_VERIFIER_STDOUT}" \
     2>"${HOT_PATH_VERIFIER_STDERR}"; then
  preflight_fail "hot-path verifier rejected replay binary"
fi
if [[ ! -s "${HOT_PATH_VERIFIER_REPORT}" ]] ||
   ! grep -Fq 'hot_path_verifier version=2 result=PASS ' \
      "${HOT_PATH_VERIFIER_STDOUT}" ||
   ! grep -Fq "schema=${PROBED_HOT_ARENA_SCHEMA}" \
      "${HOT_PATH_VERIFIER_STDOUT}" ||
   ! grep -Fq "binary_sha256=${BINARY_SHA256_BEFORE}" \
      "${HOT_PATH_VERIFIER_STDOUT}"; then
  preflight_fail "hot-path verifier output is incomplete or identifies another binary"
fi

find_cmake_build_directory() {
  local candidate=""
  candidate="$(CDPATH= cd -- "$(dirname -- "${BINARY}")" && pwd -P)" || return 1
  while [[ -n "${candidate}" ]]; do
    if [[ -r "${candidate}/CMakeCache.txt" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi
    [[ "${candidate}" == / ]] && break
    candidate="${candidate%/*}"
    [[ -n "${candidate}" ]] || candidate=/
  done
  return 1
}

CMAKE_BUILD_DIRECTORY="$(find_cmake_build_directory)" ||
  preflight_fail "cannot find CMakeCache.txt above replay binary"
CMAKE_CACHE="${CMAKE_BUILD_DIRECTORY}/CMakeCache.txt"
BINARY_SOURCE_ROOT="$(awk -F= '$1 == "CMAKE_HOME_DIRECTORY:INTERNAL" {
  print substr($0, index($0, "=") + 1); found = 1; exit
} END { if (!found) exit 1 }' "${CMAKE_CACHE}")" ||
  preflight_fail "CMake home-directory provenance is unavailable"
SOURCE_ROOT_CANONICAL="$(readlink -f -- "${BINARY_SOURCE_ROOT}")" ||
  preflight_fail "cannot resolve binary source repository root"
GIT_TOPLEVEL="$(git -C "${SOURCE_ROOT_CANONICAL}" rev-parse --show-toplevel \
  2>/dev/null)" ||
  preflight_fail "binary CMake home directory is not a Git worktree"
GIT_TOPLEVEL_CANONICAL="$(readlink -f -- "${GIT_TOPLEVEL}")" ||
  preflight_fail "cannot resolve binary source Git root"
[[ "${SOURCE_ROOT_CANONICAL}" == "${GIT_TOPLEVEL_CANONICAL}" ]] ||
  preflight_fail "binary CMake home directory is not the Git repository root"

ROOT_CANONICAL="$(readlink -f -- "${ROOT_DIR}")" ||
  preflight_fail "cannot resolve harness repository root"
OUTPUT_CANONICAL="$(readlink -f -- "${OUTPUT_DIR}")" ||
  preflight_fail "cannot resolve output directory"
GIT_PATHSPEC_ARGS=(-- .)
case "${OUTPUT_CANONICAL}" in
  "${SOURCE_ROOT_CANONICAL}"/*)
    OUTPUT_REPOSITORY_PATH="${OUTPUT_CANONICAL#"${SOURCE_ROOT_CANONICAL}"/}"
    GIT_PATHSPEC_ARGS+=(":(exclude,top,literal)${OUTPUT_REPOSITORY_PATH}")
    ;;
  *)
    OUTPUT_REPOSITORY_PATH=outside-worktree
    ;;
esac

GIT_CAPTURE_FINGERPRINT=""
GIT_CAPTURE_COMMIT=""
GIT_CAPTURE_PARENT=""
GIT_CAPTURE_TREE=""
GIT_CAPTURE_BRANCH=""
capture_git_state() {
  local phase="$1"
  local commit_file="${PROVENANCE_DIR}/git-commit-${phase}.txt"
  local parent_file="${PROVENANCE_DIR}/git-parent-${phase}.txt"
  local tree_file="${PROVENANCE_DIR}/git-tree-${phase}.txt"
  local branch_file="${PROVENANCE_DIR}/git-branch-${phase}.txt"
  local status_file="${PROVENANCE_DIR}/git-status-${phase}.txt"
  local diff_file="${PROVENANCE_DIR}/git-diff-head-${phase}.patch"
  local paths_file="${PROVENANCE_DIR}/git-untracked-paths-${phase}.nul"
  local inventory_file="${PROVENANCE_DIR}/git-untracked-blobs-${phase}.txt"
  local state_file="${PROVENANCE_DIR}/git-state-${phase}.txt"
  local path=""
  local blob=""

  if ! git -C "${SOURCE_ROOT_CANONICAL}" rev-parse --verify HEAD >"${commit_file}"; then
    return 1
  fi
  if ! git -C "${SOURCE_ROOT_CANONICAL}" rev-parse --verify HEAD^ >"${parent_file}"; then
    return 1
  fi
  if ! git -C "${SOURCE_ROOT_CANONICAL}" rev-parse --verify 'HEAD^{tree}' >"${tree_file}"; then
    return 1
  fi
  if ! git -C "${SOURCE_ROOT_CANONICAL}" symbolic-ref --quiet --short HEAD \
      >"${branch_file}"; then
    return 1
  fi
  if ! git -C "${SOURCE_ROOT_CANONICAL}" status --porcelain=v1 \
      --untracked-files=all "${GIT_PATHSPEC_ARGS[@]}" >"${status_file}"; then
    return 1
  fi
  if ! git -C "${SOURCE_ROOT_CANONICAL}" diff --binary HEAD \
      "${GIT_PATHSPEC_ARGS[@]}" >"${diff_file}"; then
    return 1
  fi
  if ! git -C "${SOURCE_ROOT_CANONICAL}" ls-files --others --exclude-standard -z \
      "${GIT_PATHSPEC_ARGS[@]}" >"${paths_file}"; then
    return 1
  fi
  : >"${inventory_file}"
  while IFS= read -r -d '' path; do
    if [[ ! -e "${SOURCE_ROOT_CANONICAL}/${path}" &&
          ! -L "${SOURCE_ROOT_CANONICAL}/${path}" ]]; then
      return 1
    fi
    if ! blob="$(git -C "${SOURCE_ROOT_CANONICAL}" hash-object --no-filters -- "${path}")"; then
      return 1
    fi
    printf '%s\t' "${blob}" >>"${inventory_file}"
    printf '%q\n' "${path}" >>"${inventory_file}"
  done <"${paths_file}"

  GIT_CAPTURE_FINGERPRINT="$({
    cat "${commit_file}"
    printf '\0'
    cat "${parent_file}"
    printf '\0'
    cat "${tree_file}"
    printf '\0'
    cat "${branch_file}"
    printf '\0'
    cat "${status_file}"
    printf '\0'
    cat "${diff_file}"
    printf '\0'
    cat "${paths_file}"
    printf '\0'
    cat "${inventory_file}"
  } | sha256sum | awk '{ print $1 }')" || return 1
  [[ "${GIT_CAPTURE_FINGERPRINT}" =~ ^[0-9a-f]{64}$ ]] || return 1
  GIT_CAPTURE_COMMIT="$(<"${commit_file}")"
  GIT_CAPTURE_PARENT="$(<"${parent_file}")"
  GIT_CAPTURE_TREE="$(<"${tree_file}")"
  GIT_CAPTURE_BRANCH="$(<"${branch_file}")"
  [[ -n "${GIT_CAPTURE_BRANCH}" ]] || return 1
  {
    echo "commit=${GIT_CAPTURE_COMMIT}"
    echo "parent=${GIT_CAPTURE_PARENT}"
    echo "tree=${GIT_CAPTURE_TREE}"
    echo "branch=${GIT_CAPTURE_BRANCH}"
    echo "fingerprint_sha256=${GIT_CAPTURE_FINGERPRINT}"
    echo "status_sha256=$(sha256sum -- "${status_file}" | awk '{ print $1 }')"
    echo "diff_head_sha256=$(sha256sum -- "${diff_file}" | awk '{ print $1 }')"
    echo "untracked_paths_sha256=$(sha256sum -- "${paths_file}" | awk '{ print $1 }')"
    echo "untracked_blobs_sha256=$(sha256sum -- "${inventory_file}" | awk '{ print $1 }')"
    echo "artifact_path_excluded=${OUTPUT_REPOSITORY_PATH}"
  } >"${state_file}"
  return 0
}

if ! capture_git_state before; then
  preflight_fail "cannot capture complete pre-run Git state"
fi
GIT_FINGERPRINT_BEFORE="${GIT_CAPTURE_FINGERPRINT}"
GIT_COMMIT_BEFORE="${GIT_CAPTURE_COMMIT}"
GIT_PARENT_BEFORE="${GIT_CAPTURE_PARENT}"
GIT_TREE_BEFORE="${GIT_CAPTURE_TREE}"
GIT_BRANCH_BEFORE="${GIT_CAPTURE_BRANCH}"
cp -- "${PROVENANCE_DIR}/git-status-before.txt" \
  "${OUTPUT_DIR}/git-status-porcelain.txt"
GIT_STATUS_BEFORE="$(<"${PROVENANCE_DIR}/git-status-before.txt")"
if [[ -n "${GIT_STATUS_BEFORE}" ]]; then
  GIT_DIRTY=1
else
  GIT_DIRTY=0
fi
[[ "${GIT_DIRTY}" -eq 0 ]] ||
  preflight_fail "live acceptance requires a clean Git worktree and index"
if [[ "${PROBED_HOT_ARENA_SCHEMA}" == branch5_native_v1 ]]; then
  BRANCH5_PINNED_COMMIT=324d81a15ee52cc72f68873a1ced122923406df2
  BRANCH5_COMPATIBILITY_TREE=492938730e6db91e84bdb1f8e25152536e81dbc0
  [[ "${GIT_PARENT_BEFORE}" == "${BRANCH5_PINNED_COMMIT}" ]] ||
    preflight_fail "branch5 compatibility commit is not directly based on ${BRANCH5_PINNED_COMMIT}"
  [[ "$(git -C "${SOURCE_ROOT_CANONICAL}" rev-list --count \
       "${BRANCH5_PINNED_COMMIT}..${GIT_COMMIT_BEFORE}")" == 1 ]] ||
    preflight_fail "branch5 baseline must be exactly one compatibility commit above the pinned implementation"
  [[ "${GIT_TREE_BEFORE}" == "${BRANCH5_COMPATIBILITY_TREE}" ]] ||
    preflight_fail "branch5 compatibility tree differs from the reviewed native port"
fi

cache_value() {
  local key="$1"
  awk -F= -v prefix="${key}:" 'index($1, prefix) == 1 {
    print substr($0, index($0, "=") + 1)
    found = 1
    exit
  }
  END { if (!found) exit 1 }' "${CMAKE_CACHE}"
}
cmake_set_value() {
  local source="$1"
  local key="$2"
  awk -v prefix="set(""${key}"" \"" 'index($0, prefix) == 1 {
    value = substr($0, length(prefix) + 1)
    sub(/\"\)$/, "", value)
    print value
    found = 1
    exit
  }
  END { if (!found) exit 1 }' "${source}"
}

CMAKE_BUILD_TYPE="$(cache_value CMAKE_BUILD_TYPE 2>/dev/null || true)"
[[ "${CMAKE_BUILD_TYPE}" == Release ]] ||
  preflight_fail "live acceptance requires CMAKE_BUILD_TYPE=Release"
CMAKE_CXX_COMPILER="$(cache_value CMAKE_CXX_COMPILER 2>/dev/null || true)"
[[ -n "${CMAKE_CXX_COMPILER}" && -x "${CMAKE_CXX_COMPILER}" ]] ||
  preflight_fail "CMake C++ compiler provenance is unavailable or not executable"
CMAKE_GENERATOR="$(cache_value CMAKE_GENERATOR 2>/dev/null || true)"
CMAKE_MAKE_PROGRAM="$(cache_value CMAKE_MAKE_PROGRAM 2>/dev/null || true)"
[[ -n "${CMAKE_GENERATOR}" ]] ||
  preflight_fail "CMake generator provenance is unavailable"
[[ -n "${CMAKE_MAKE_PROGRAM}" && -x "${CMAKE_MAKE_PROGRAM}" ]] ||
  preflight_fail "CMake build-tool provenance is unavailable or not executable"
CMAKE_CXX_FLAGS="$(cache_value CMAKE_CXX_FLAGS 2>/dev/null || true)"
CMAKE_CXX_FLAGS_RELEASE="$(cache_value CMAKE_CXX_FLAGS_RELEASE 2>/dev/null || true)"
CMAKE_EXE_LINKER_FLAGS="$(cache_value CMAKE_EXE_LINKER_FLAGS 2>/dev/null || true)"
CMAKE_EXE_LINKER_FLAGS_RELEASE="$(cache_value CMAKE_EXE_LINKER_FLAGS_RELEASE 2>/dev/null || true)"
CMAKE_STATIC_LINKER_FLAGS="$(cache_value CMAKE_STATIC_LINKER_FLAGS 2>/dev/null || true)"
CMAKE_STATIC_LINKER_FLAGS_RELEASE="$(cache_value CMAKE_STATIC_LINKER_FLAGS_RELEASE 2>/dev/null || true)"

CMAKE_CXX_COMPILER_METADATA=""
while IFS= read -r -d '' compiler_metadata; do
  CMAKE_CXX_COMPILER_METADATA="${compiler_metadata}"
  break
done < <(find "${CMAKE_BUILD_DIRECTORY}/CMakeFiles" -mindepth 2 -maxdepth 2 \
               -type f -name CMakeCXXCompiler.cmake -print0 2>/dev/null)
[[ -n "${CMAKE_CXX_COMPILER_METADATA}" ]] ||
  preflight_fail "CMake C++ compiler metadata is unavailable"
CMAKE_CXX_COMPILER_ID="$(cmake_set_value "${CMAKE_CXX_COMPILER_METADATA}" \
                                      CMAKE_CXX_COMPILER_ID 2>/dev/null || true)"
CMAKE_CXX_COMPILER_VERSION="$(cmake_set_value "${CMAKE_CXX_COMPILER_METADATA}" \
                                           CMAKE_CXX_COMPILER_VERSION 2>/dev/null || true)"
[[ -n "${CMAKE_CXX_COMPILER_ID}" && -n "${CMAKE_CXX_COMPILER_VERSION}" ]] ||
  preflight_fail "CMake compiler ID/version provenance is unavailable"

for provenance_value in "${CMAKE_GENERATOR}" "${CMAKE_MAKE_PROGRAM}" \
                        "${CMAKE_CXX_COMPILER}" "${CMAKE_CXX_FLAGS}" \
                        "${CMAKE_CXX_FLAGS_RELEASE}" \
                        "${CMAKE_EXE_LINKER_FLAGS}" \
                        "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" \
                        "${CMAKE_STATIC_LINKER_FLAGS}" \
                        "${CMAKE_STATIC_LINKER_FLAGS_RELEASE}"; do
  if [[ "${provenance_value}" == *$'\n'* ||
        "${provenance_value}" == *$'\r'* ||
        "${provenance_value}" == *$'\t'* ]]; then
    preflight_fail "CMake provenance fields may not contain control characters"
  fi
done

for required_tool in cmake git tar mktemp env; do
  command -v "${required_tool}" >/dev/null 2>&1 ||
    preflight_fail "${required_tool} is required for clean-source binary attestation"
done
CMAKE_COMMAND="$(readlink -f -- "$(command -v cmake)")" ||
  preflight_fail "cannot resolve cmake for clean-source binary attestation"
GIT_COMMAND="$(readlink -f -- "$(command -v git)")" ||
  preflight_fail "cannot resolve git for clean-source binary attestation"
TAR_COMMAND="$(readlink -f -- "$(command -v tar)")" ||
  preflight_fail "cannot resolve tar for clean-source binary attestation"
ENV_COMMAND="$(readlink -f -- "$(command -v env)")" ||
  preflight_fail "cannot resolve env for clean-source binary attestation"
SOURCE_BUILD_EPOCH="$("${GIT_COMMAND}" -C "${SOURCE_ROOT_CANONICAL}" show \
  -s --format=%ct "${GIT_COMMIT_BEFORE}")" ||
  preflight_fail "cannot read clean-source commit timestamp"
is_uint "${SOURCE_BUILD_EPOCH}" ||
  preflight_fail "clean-source commit timestamp is not an unsigned integer"

SOURCE_BUILD_PROVENANCE_DIR="${BUILD_PROVENANCE_DIR}/source-build"
mkdir -p "${SOURCE_BUILD_PROVENANCE_DIR}"
SOURCE_TREE_ARCHIVE="${SOURCE_BUILD_PROVENANCE_DIR}/source-tree.tar"
SOURCE_ARCHIVE_COMMAND=(
  "${GIT_COMMAND}" -C "${SOURCE_ROOT_CANONICAL}" archive
  --format=tar "--output=${SOURCE_TREE_ARCHIVE}" "${GIT_COMMIT_BEFORE}"
)
write_command_file "${SOURCE_BUILD_PROVENANCE_DIR}/source-archive.command.txt" \
  "${SOURCE_ARCHIVE_COMMAND[@]}"
write_argv_file "${SOURCE_BUILD_PROVENANCE_DIR}/source-archive.argv" \
  "${SOURCE_ARCHIVE_COMMAND[@]}"
if ! "${SOURCE_ARCHIVE_COMMAND[@]}" \
     >"${SOURCE_BUILD_PROVENANCE_DIR}/source-archive.stdout.log" \
     2>"${SOURCE_BUILD_PROVENANCE_DIR}/source-archive.stderr.log"; then
  preflight_fail "cannot export the verified clean source commit"
fi
if ! capture_file_state "${SOURCE_TREE_ARCHIVE}" \
     "${SOURCE_BUILD_PROVENANCE_DIR}/source-tree-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
SOURCE_TREE_ARCHIVE_SHA256="${FILE_STATE_SHA256}"
SOURCE_TREE_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"

SOURCE_BUILD_TEMP_ROOT="$(mktemp -d \
  "${TMPDIR:-/tmp}/astra-order-book-source-build.XXXXXX")" ||
  preflight_fail "cannot create temporary clean-source build root"
SOURCE_BUILD_TEMP_SOURCE="${SOURCE_BUILD_TEMP_ROOT}/source"
SOURCE_BUILD_TEMP_DIRECTORY="${SOURCE_BUILD_TEMP_ROOT}/build"
SOURCE_BUILD_TEMP_HOME="${SOURCE_BUILD_TEMP_ROOT}/home"
SOURCE_BUILD_TEMP_TMP="${SOURCE_BUILD_TEMP_ROOT}/tmp"
mkdir -p "${SOURCE_BUILD_TEMP_SOURCE}" "${SOURCE_BUILD_TEMP_DIRECTORY}" \
         "${SOURCE_BUILD_TEMP_HOME}" "${SOURCE_BUILD_TEMP_TMP}"

SOURCE_EXTRACT_COMMAND=(
  "${TAR_COMMAND}" -xf "${SOURCE_TREE_ARCHIVE}"
  -C "${SOURCE_BUILD_TEMP_SOURCE}"
)
write_command_file "${SOURCE_BUILD_PROVENANCE_DIR}/source-extract.command.txt" \
  "${SOURCE_EXTRACT_COMMAND[@]}"
write_argv_file "${SOURCE_BUILD_PROVENANCE_DIR}/source-extract.argv" \
  "${SOURCE_EXTRACT_COMMAND[@]}"
if ! "${SOURCE_EXTRACT_COMMAND[@]}" \
     >"${SOURCE_BUILD_PROVENANCE_DIR}/source-extract.stdout.log" \
     2>"${SOURCE_BUILD_PROVENANCE_DIR}/source-extract.stderr.log"; then
  preflight_fail "cannot extract the verified clean source archive"
fi

SOURCE_BUILD_ENVIRONMENT=(
  "${ENV_COMMAND}" -i
  "HOME=${SOURCE_BUILD_TEMP_HOME}"
  "PATH=${PATH:-/usr/bin:/bin}"
  "TMPDIR=${SOURCE_BUILD_TEMP_TMP}"
  LC_ALL=C
  "SOURCE_DATE_EPOCH=${SOURCE_BUILD_EPOCH}"
  ZERO_AR_DATE=1
)
SOURCE_CONFIGURE_COMMAND=(
  "${SOURCE_BUILD_ENVIRONMENT[@]}"
  "${CMAKE_COMMAND}"
  -S "${SOURCE_BUILD_TEMP_SOURCE}"
  -B "${SOURCE_BUILD_TEMP_DIRECTORY}"
  -G "${CMAKE_GENERATOR}"
  --no-warn-unused-cli
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
  "-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}"
  "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
  "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=${CMAKE_EXE_LINKER_FLAGS_RELEASE}"
  "-DCMAKE_STATIC_LINKER_FLAGS=${CMAKE_STATIC_LINKER_FLAGS}"
  "-DCMAKE_STATIC_LINKER_FLAGS_RELEASE=${CMAKE_STATIC_LINKER_FLAGS_RELEASE}"
  -DASTRA_BUILD_APPS=OFF
  -DASTRA_BUILD_TESTS=OFF
  -DASTRA_BUILD_BENCHMARKS=ON
  -DASTRA_ENABLE_DPDK=OFF
  -DASTRA_ENABLE_IPO=OFF
)
write_command_file "${SOURCE_BUILD_PROVENANCE_DIR}/configure.command.txt" \
  "${SOURCE_CONFIGURE_COMMAND[@]}"
write_argv_file "${SOURCE_BUILD_PROVENANCE_DIR}/configure.argv" \
  "${SOURCE_CONFIGURE_COMMAND[@]}"
if ! "${SOURCE_CONFIGURE_COMMAND[@]}" \
     >"${SOURCE_BUILD_PROVENANCE_DIR}/configure.stdout.log" \
     2>"${SOURCE_BUILD_PROVENANCE_DIR}/configure.stderr.log"; then
  preflight_fail "fresh clean-source CMake configure failed"
fi

SOURCE_BUILD_TARGET=astra_itch_book_replay_benchmark
SOURCE_TARGET_BUILD_COMMAND=(
  "${SOURCE_BUILD_ENVIRONMENT[@]}"
  "${CMAKE_COMMAND}"
  --build "${SOURCE_BUILD_TEMP_DIRECTORY}"
  --target "${SOURCE_BUILD_TARGET}"
  --clean-first
  --verbose
)
write_command_file "${SOURCE_BUILD_PROVENANCE_DIR}/target-build.command.txt" \
  "${SOURCE_TARGET_BUILD_COMMAND[@]}"
write_argv_file "${SOURCE_BUILD_PROVENANCE_DIR}/target-build.argv" \
  "${SOURCE_TARGET_BUILD_COMMAND[@]}"
if ! "${SOURCE_TARGET_BUILD_COMMAND[@]}" \
     >"${SOURCE_BUILD_PROVENANCE_DIR}/target-build.stdout.log" \
     2>"${SOURCE_BUILD_PROVENANCE_DIR}/target-build.stderr.log"; then
  preflight_fail "fresh clean-source target build failed"
fi

SOURCE_BUILD_BINARY="${SOURCE_BUILD_TEMP_DIRECTORY}/benchmarks/astra_itch_book_replay_benchmark"
[[ -x "${SOURCE_BUILD_BINARY}" && -f "${SOURCE_BUILD_BINARY}" ]] ||
  preflight_fail "fresh clean-source build did not produce the replay target"
if ! capture_file_state "${SOURCE_BUILD_BINARY}" \
     "${SOURCE_BUILD_PROVENANCE_DIR}/fresh-binary-source.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
SOURCE_BUILD_BINARY_SHA256="${FILE_STATE_SHA256}"
SOURCE_BUILD_BINARY_ARCHIVE="${SOURCE_BUILD_PROVENANCE_DIR}/fresh-built-binary"
if ! cp --preserve=mode,timestamps -- "${SOURCE_BUILD_BINARY}" \
     "${SOURCE_BUILD_BINARY_ARCHIVE}"; then
  preflight_fail "cannot archive fresh clean-source replay binary"
fi
if ! capture_file_state "${SOURCE_BUILD_BINARY_ARCHIVE}" \
     "${SOURCE_BUILD_PROVENANCE_DIR}/fresh-binary-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
SOURCE_BUILD_BINARY_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
SOURCE_BUILD_BINARY_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
if [[ "${SOURCE_BUILD_BINARY_SHA256}" != \
      "${SOURCE_BUILD_BINARY_ARCHIVE_SHA256_BEFORE}" ]]; then
  preflight_fail "archived fresh clean-source binary differs from build output"
fi

SOURCE_BUILD_CMAKE_CACHE="${SOURCE_BUILD_TEMP_DIRECTORY}/CMakeCache.txt"
[[ -r "${SOURCE_BUILD_CMAKE_CACHE}" && -f "${SOURCE_BUILD_CMAKE_CACHE}" ]] ||
  preflight_fail "fresh clean-source CMake cache is unavailable"
SOURCE_BUILD_CMAKE_CACHE_ARCHIVE="${SOURCE_BUILD_PROVENANCE_DIR}/CMakeCache.txt"
cp -- "${SOURCE_BUILD_CMAKE_CACHE}" "${SOURCE_BUILD_CMAKE_CACHE_ARCHIVE}"
if ! capture_file_state "${SOURCE_BUILD_CMAKE_CACHE_ARCHIVE}" \
     "${SOURCE_BUILD_PROVENANCE_DIR}/cmake-cache-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
SOURCE_BUILD_CMAKE_CACHE_SHA256="${FILE_STATE_SHA256}"
SOURCE_BUILD_CMAKE_CACHE_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
for build_file in compile_commands.json build.ninja Makefile; do
  if [[ -r "${SOURCE_BUILD_TEMP_DIRECTORY}/${build_file}" ]]; then
    cp -- "${SOURCE_BUILD_TEMP_DIRECTORY}/${build_file}" \
      "${SOURCE_BUILD_PROVENANCE_DIR}/${build_file}"
  fi
done
if [[ -r "${SOURCE_BUILD_TEMP_DIRECTORY}/CMakeFiles/rules.ninja" ]]; then
  cp -- "${SOURCE_BUILD_TEMP_DIRECTORY}/CMakeFiles/rules.ninja" \
    "${SOURCE_BUILD_PROVENANCE_DIR}/rules.ninja"
fi

if ! capture_file_state "${BINARY}" \
     "${PROVENANCE_DIR}/binary-source-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
BINARY_SHA256_BEFORE="${FILE_STATE_SHA256}"
BINARY_STAT_BEFORE="${FILE_STATE_STAT}"
BINARY_SIZE_BYTES="${FILE_STATE_SIZE}"
if [[ "${BINARY_SHA256_BEFORE}" != "${SUPPLIED_BINARY_SHA256}" ||
      "${BINARY_STAT_BEFORE}" != "${SUPPLIED_BINARY_STAT}" ]]; then
  preflight_fail "supplied replay binary changed during clean-source rebuild"
fi
if ! binary_matches_fresh_source_build \
     "${BINARY}" "${SUPPLIED_BINARY_SHA256}" \
     "${SOURCE_BUILD_BINARY}" "${SOURCE_BUILD_BINARY_SHA256}"; then
  preflight_fail "supplied replay binary is not byte-identical to the fresh clean-source build"
fi

if ! capture_git_state post-build; then
  preflight_fail "cannot capture post-build Git state"
fi
SOURCE_BUILD_GIT_FINGERPRINT_AFTER="${GIT_CAPTURE_FINGERPRINT}"
if [[ "${GIT_CAPTURE_COMMIT}" != "${GIT_COMMIT_BEFORE}" ||
      "${GIT_CAPTURE_PARENT}" != "${GIT_PARENT_BEFORE}" ||
      "${GIT_CAPTURE_TREE}" != "${GIT_TREE_BEFORE}" ||
      "${GIT_CAPTURE_BRANCH}" != "${GIT_BRANCH_BEFORE}" ||
      "${SOURCE_BUILD_GIT_FINGERPRINT_AFTER}" != \
        "${GIT_FINGERPRINT_BEFORE}" ]]; then
  preflight_fail "Git identity or worktree content changed during clean-source rebuild"
fi

SOURCE_ARCHIVE_ARGV_SHA256="$(sha256sum -- \
  "${SOURCE_BUILD_PROVENANCE_DIR}/source-archive.argv" | awk '{ print $1 }')"
SOURCE_EXTRACT_ARGV_SHA256="$(sha256sum -- \
  "${SOURCE_BUILD_PROVENANCE_DIR}/source-extract.argv" | awk '{ print $1 }')"
SOURCE_CONFIGURE_ARGV_SHA256="$(sha256sum -- \
  "${SOURCE_BUILD_PROVENANCE_DIR}/configure.argv" | awk '{ print $1 }')"
SOURCE_TARGET_BUILD_ARGV_SHA256="$(sha256sum -- \
  "${SOURCE_BUILD_PROVENANCE_DIR}/target-build.argv" | awk '{ print $1 }')"
SOURCE_BUILD_ATTESTATION="${SOURCE_BUILD_PROVENANCE_DIR}/source-build-attestation.txt"
{
  echo "attestation_version=1"
  echo "mode=git_archive_fresh_cmake_clean_first_v1"
  echo "environment_policy=empty_environment_recorded_toolchain_v1"
  echo "target=${SOURCE_BUILD_TARGET}"
  echo "source_commit=${GIT_COMMIT_BEFORE}"
  echo "source_parent=${GIT_PARENT_BEFORE}"
  echo "source_tree=${GIT_TREE_BEFORE}"
  echo "source_branch=${GIT_BRANCH_BEFORE}"
  echo "source_fingerprint_before=${GIT_FINGERPRINT_BEFORE}"
  echo "source_fingerprint_after=${SOURCE_BUILD_GIT_FINGERPRINT_AFTER}"
  echo "source_date_epoch=${SOURCE_BUILD_EPOCH}"
  echo "git_command=${GIT_COMMAND}"
  echo "tar_command=${TAR_COMMAND}"
  echo "cmake_command=${CMAKE_COMMAND}"
  echo "env_command=${ENV_COMMAND}"
  echo "source_root=${SOURCE_ROOT_CANONICAL}"
  echo "source_archive_output=${SOURCE_TREE_ARCHIVE}"
  echo "temporary_root=${SOURCE_BUILD_TEMP_ROOT}"
  echo "temporary_source=${SOURCE_BUILD_TEMP_SOURCE}"
  echo "temporary_build=${SOURCE_BUILD_TEMP_DIRECTORY}"
  echo "source_archive_sha256=${SOURCE_TREE_ARCHIVE_SHA256}"
  echo "source_archive_argv_sha256=${SOURCE_ARCHIVE_ARGV_SHA256}"
  echo "source_extract_argv_sha256=${SOURCE_EXTRACT_ARGV_SHA256}"
  echo "configure_argv_sha256=${SOURCE_CONFIGURE_ARGV_SHA256}"
  echo "target_build_argv_sha256=${SOURCE_TARGET_BUILD_ARGV_SHA256}"
  echo "fresh_cmake_cache_sha256=${SOURCE_BUILD_CMAKE_CACHE_SHA256}"
  echo "supplied_binary_sha256=${SUPPLIED_BINARY_SHA256}"
  echo "fresh_binary_sha256=${SOURCE_BUILD_BINARY_SHA256}"
  echo "fresh_binary_archive_sha256=${SOURCE_BUILD_BINARY_ARCHIVE_SHA256_BEFORE}"
  echo "configure_exit_code=0"
  echo "target_build_exit_code=0"
  echo "result=PASS"
  echo "error=none"
} >"${SOURCE_BUILD_ATTESTATION}"
if ! capture_file_state "${SOURCE_BUILD_ATTESTATION}" \
     "${SOURCE_BUILD_PROVENANCE_DIR}/source-build-attestation-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
SOURCE_BUILD_ATTESTATION_SHA256_BEFORE="${FILE_STATE_SHA256}"
SOURCE_BUILD_ATTESTATION_STAT_BEFORE="${FILE_STATE_STAT}"

if ! cleanup_source_build_temp; then
  preflight_fail "cannot remove temporary clean-source build root"
fi

COMPILER_VERSION_FILE="${BUILD_PROVENANCE_DIR}/compiler-version.txt"
if ! "${CMAKE_CXX_COMPILER}" --version >"${COMPILER_VERSION_FILE}" 2>&1 ||
   [[ ! -s "${COMPILER_VERSION_FILE}" ]]; then
  preflight_fail "CMake C++ compiler --version failed"
fi
if ! capture_file_state "${CMAKE_CACHE}" \
     "${BUILD_PROVENANCE_DIR}/cmake-cache-source-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
CMAKE_CACHE_SHA256_BEFORE="${FILE_STATE_SHA256}"
CMAKE_CACHE_STAT_BEFORE="${FILE_STATE_STAT}"
CMAKE_CACHE_ARCHIVE="${BUILD_PROVENANCE_DIR}/CMakeCache.txt"
cp -- "${CMAKE_CACHE}" "${CMAKE_CACHE_ARCHIVE}"
if ! capture_file_state "${CMAKE_CACHE_ARCHIVE}" \
     "${BUILD_PROVENANCE_DIR}/cmake-cache-archive-before.state"; then
  preflight_fail "${FILE_STATE_ERROR}"
fi
CMAKE_CACHE_ARCHIVE_SHA256_BEFORE="${FILE_STATE_SHA256}"
CMAKE_CACHE_ARCHIVE_STAT_BEFORE="${FILE_STATE_STAT}"
if [[ "${CMAKE_CACHE_ARCHIVE_SHA256_BEFORE}" != \
      "${CMAKE_CACHE_SHA256_BEFORE}" ]]; then
  preflight_fail "archived CMakeCache differs from build CMakeCache"
fi
cp -- "${CMAKE_CXX_COMPILER_METADATA}" \
  "${BUILD_PROVENANCE_DIR}/CMakeCXXCompiler.cmake"
for build_file in compile_commands.json build.ninja Makefile; do
  if [[ -r "${CMAKE_BUILD_DIRECTORY}/${build_file}" ]]; then
    cp -- "${CMAKE_BUILD_DIRECTORY}/${build_file}" \
      "${BUILD_PROVENANCE_DIR}/${build_file}"
  fi
done
if [[ -r "${CMAKE_BUILD_DIRECTORY}/CMakeFiles/rules.ninja" ]]; then
  cp -- "${CMAKE_BUILD_DIRECTORY}/CMakeFiles/rules.ninja" \
    "${BUILD_PROVENANCE_DIR}/rules.ninja"
fi

TARGET_BUILD_COMMANDS_FILE=unavailable
if [[ "${CMAKE_GENERATOR}" == Ninja* &&
      -n "${CMAKE_MAKE_PROGRAM}" && -x "${CMAKE_MAKE_PROGRAM}" ]]; then
  TARGET_BUILD_COMMANDS_FILE="${BUILD_PROVENANCE_DIR}/target-build-commands.txt"
  if ! "${CMAKE_MAKE_PROGRAM}" -C "${CMAKE_BUILD_DIRECTORY}" -t commands \
      astra_itch_book_replay_benchmark >"${TARGET_BUILD_COMMANDS_FILE}" 2>&1 ||
     [[ ! -s "${TARGET_BUILD_COMMANDS_FILE}" ]]; then
    preflight_fail "cannot retain Ninja commands for replay benchmark target"
  fi
fi
{
  printf 'build_directory='; printf '%q\n' "${CMAKE_BUILD_DIRECTORY}"
  echo "cmake_build_type=${CMAKE_BUILD_TYPE}"
  echo "cmake_generator=${CMAKE_GENERATOR}"
  printf 'cmake_make_program='; printf '%q\n' "${CMAKE_MAKE_PROGRAM}"
  printf 'cmake_cxx_compiler='; printf '%q\n' "${CMAKE_CXX_COMPILER}"
  echo "cmake_cxx_compiler_id=${CMAKE_CXX_COMPILER_ID}"
  echo "cmake_cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION}"
  printf 'cmake_cxx_flags='; printf '%q\n' "${CMAKE_CXX_FLAGS}"
  printf 'cmake_cxx_flags_release='; printf '%q\n' "${CMAKE_CXX_FLAGS_RELEASE}"
  printf 'cmake_exe_linker_flags='; printf '%q\n' "${CMAKE_EXE_LINKER_FLAGS}"
  printf 'cmake_exe_linker_flags_release='; printf '%q\n' "${CMAKE_EXE_LINKER_FLAGS_RELEASE}"
  printf 'cmake_static_linker_flags='; printf '%q\n' "${CMAKE_STATIC_LINKER_FLAGS}"
  printf 'cmake_static_linker_flags_release='; printf '%q\n' "${CMAKE_STATIC_LINKER_FLAGS_RELEASE}"
  echo "source_build_attestation_version=1"
  echo "source_build_mode=git_archive_fresh_cmake_clean_first_v1"
  echo "source_build_environment_policy=empty_environment_recorded_toolchain_v1"
  echo "source_build_target=${SOURCE_BUILD_TARGET}"
  echo "target_build_commands_file=${TARGET_BUILD_COMMANDS_FILE}"
} >"${BUILD_PROVENANCE_DIR}/build-provenance.txt"

build_base_command
build_plan_command
benchmark_help=""
if benchmark_help="$("${BINARY}" --help 2>&1)"; then
  for required_flag in --prefault --sample-every --warmup-book-messages \
                       --max-p50-ns --max-p99-ns --max-p99-9-ns \
                       --min-samples --expect-records --expect-bytes \
                       --sample-capacity \
                       --start-gate-file --start-gate-timeout-ms \
                       --mutation-digest --expect-mutation-digest \
                       --expect-semantic-mutation-digest \
                       --storage-plan-only --require-zero-post-warmup-faults; do
    case "${benchmark_help}" in
      *"${required_flag}"*) ;;
      *) preflight_fail "replay binary help is missing ${required_flag}" ;;
    esac
  done
else
  preflight_fail "replay binary --help failed"
fi
if [[ "${PROBED_HOT_ARENA_SCHEMA}" == redesign_v1 ]]; then
  for required_flag in --direct-order-slots --fallback-buckets \
                       --price-page-capacity --capacity-profile-name \
                       --capacity-evidence-file \
                       --capacity-evidence-sha256; do
    case "${benchmark_help}" in
      *"${required_flag}"*) ;;
      *) preflight_fail "redesign replay binary help is missing ${required_flag}" ;;
    esac
  done
fi
printf '%s\n' "${benchmark_help}" >"${OUTPUT_DIR}/benchmark-help.txt"

BINARY_SHA256="${BINARY_SHA256_BEFORE}"
TRACE_SHA256="${TRACE_SHA256_BEFORE}"

RECORD_FIELD_VALUE=""
read_record_field() {
  local source="$1"
  local record_name="$2"
  local key="$3"
  RECORD_FIELD_VALUE="$(awk -v record="${record_name}" -v wanted="${key}" '
    $1 == record {
      ++lines
      prefix = wanted "="
      for (field_number = 2; field_number <= NF; ++field_number) {
        token = $field_number
        if (substr(token, 1, length(prefix)) == prefix) {
          value = substr(token, length(prefix) + 1)
          ++matches
        }
      }
    }
    END {
      if (lines != 1 || matches != 1)
        exit 1
      print value
    }
  ' "${source}")" || return 1
  return 0
}

PLAN_STDOUT="${OUTPUT_DIR}/storage-plan.stdout.log"
PLAN_STDERR="${OUTPUT_DIR}/storage-plan.stderr.log"
printf '%q ' "${PLAN_COMMAND[@]}" >"${OUTPUT_DIR}/storage-plan.command.txt"
printf '\n' >>"${OUTPUT_DIR}/storage-plan.command.txt"
if "${PLAN_COMMAND[@]}" >"${PLAN_STDOUT}" 2>"${PLAN_STDERR}"; then
  PLAN_EXIT=0
else
  PLAN_EXIT=$?
fi
cat "${PLAN_STDOUT}"
if [[ -s "${PLAN_STDERR}" ]]; then
  cat "${PLAN_STDERR}" >&2
fi
if [[ "${PLAN_EXIT}" -ne 0 ]]; then
  preflight_fail "storage-plan command exited ${PLAN_EXIT}"
fi
if ! cmp -s -- "${HOT_ARENA_SCHEMA_PROBE_STDOUT}" "${PLAN_STDOUT}" ||
   ! cmp -s -- "${HOT_ARENA_SCHEMA_PROBE_STDERR}" "${PLAN_STDERR}"; then
  preflight_fail "storage-plan output changed between identical schema-probe and admission commands"
fi

if ! read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
     hot_arena_schema; then
  preflight_fail "storage plan lacks one unambiguous hot_arena_schema field"
fi
PLAN_HOT_ARENA_SCHEMA="${RECORD_FIELD_VALUE}"
if [[ "${PLAN_HOT_ARENA_SCHEMA}" != "${PROBED_HOT_ARENA_SCHEMA}" ]]; then
  preflight_fail "hot-arena schema changed between probe and storage plan"
fi
if ! configure_hot_arena_schema "${PLAN_HOT_ARENA_SCHEMA}"; then
  preflight_fail "unsupported hot-arena schema: ${PLAN_HOT_ARENA_SCHEMA:-missing}"
fi
PLAN_CAPACITY_PROFILE_BOUND=not_applicable
PLAN_CAPACITY_EVIDENCE_SCHEMA=not_applicable
PLAN_CAPACITY_PROFILE_NAME=not_applicable
PLAN_CAPACITY_EVIDENCE_SHA256=not_applicable
PLAN_CAPACITY_CORPUS_MANIFEST_SHA256=not_applicable
PLAN_CAPACITY_PROFILER_SHA256=not_applicable
PLAN_CAPACITY_PROFILED_MAX_ORDER_REF=not_applicable
PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES=not_applicable
PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM=not_applicable
PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM=not_applicable
PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM=not_applicable
PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM=not_applicable
if [[ "${HOT_ARENA_POLICY}" == redesign_exact_v1 ]]; then
  for plan_key in capacity_profile_bound capacity_evidence_schema \
                  capacity_profile_name capacity_evidence_sha256 \
                  capacity_corpus_manifest_sha256 capacity_profiler_sha256 \
                  capacity_profiled_max_order_ref \
                  capacity_profiled_unique_price_pages \
                  capacity_minimum_direct_order_headroom \
                  capacity_effective_direct_order_headroom \
                  capacity_minimum_price_page_headroom \
                  capacity_effective_price_page_headroom; do
    if ! read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
         "${plan_key}"; then
      preflight_fail "storage plan lacks one unambiguous ${plan_key} field"
    fi
    case "${plan_key}" in
      capacity_profile_bound)
        PLAN_CAPACITY_PROFILE_BOUND="${RECORD_FIELD_VALUE}" ;;
      capacity_evidence_schema)
        PLAN_CAPACITY_EVIDENCE_SCHEMA="${RECORD_FIELD_VALUE}" ;;
      capacity_profile_name)
        PLAN_CAPACITY_PROFILE_NAME="${RECORD_FIELD_VALUE}" ;;
      capacity_evidence_sha256)
        PLAN_CAPACITY_EVIDENCE_SHA256="${RECORD_FIELD_VALUE}" ;;
      capacity_corpus_manifest_sha256)
        PLAN_CAPACITY_CORPUS_MANIFEST_SHA256="${RECORD_FIELD_VALUE}" ;;
      capacity_profiler_sha256)
        PLAN_CAPACITY_PROFILER_SHA256="${RECORD_FIELD_VALUE}" ;;
      capacity_profiled_max_order_ref)
        PLAN_CAPACITY_PROFILED_MAX_ORDER_REF="${RECORD_FIELD_VALUE}" ;;
      capacity_profiled_unique_price_pages)
        PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES="${RECORD_FIELD_VALUE}" ;;
      capacity_minimum_direct_order_headroom)
        PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM="${RECORD_FIELD_VALUE}" ;;
      capacity_effective_direct_order_headroom)
        PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM="${RECORD_FIELD_VALUE}" ;;
      capacity_minimum_price_page_headroom)
        PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM="${RECORD_FIELD_VALUE}" ;;
      capacity_effective_price_page_headroom)
        PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM="${RECORD_FIELD_VALUE}" ;;
    esac
  done
  [[ "${PLAN_CAPACITY_PROFILE_BOUND}" == 1 ]] ||
    preflight_fail "redesign acceptance requires a bound capacity profile"
  if ! is_sha256 "${PLAN_CAPACITY_EVIDENCE_SHA256}" ||
     ! is_sha256 "${PLAN_CAPACITY_CORPUS_MANIFEST_SHA256}" ||
     ! is_sha256 "${PLAN_CAPACITY_PROFILER_SHA256}"; then
    preflight_fail "redesign capacity profile contains malformed provenance hashes"
  fi
  for plan_value in "${PLAN_CAPACITY_PROFILED_MAX_ORDER_REF}" \
                    "${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}" \
                    "${PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM}" \
                    "${PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM}" \
                    "${PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM}" \
                    "${PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM}"; do
    if ! is_uint "${plan_value}" ||
       [[ "$(normalize_uint "${plan_value}")" == 0 ]]; then
      preflight_fail "redesign capacity profile contains a nonpositive demand/headroom value"
    fi
  done
  if [[ -n "${CAPACITY_EVIDENCE_FILE}" ]]; then
    [[ "${PLAN_CAPACITY_EVIDENCE_SCHEMA}" == \
       astra_book_capacity_evidence_v1 ]] ||
      preflight_fail "custom capacity profile reported the wrong evidence schema"
    [[ "${PLAN_CAPACITY_PROFILE_NAME}" == "${CAPACITY_PROFILE_NAME}" ]] ||
      preflight_fail "custom capacity profile name differs from the request"
    [[ "${PLAN_CAPACITY_EVIDENCE_SHA256}" == \
       "${CAPACITY_EVIDENCE_SHA256}" &&
       "${PLAN_CAPACITY_EVIDENCE_SHA256}" == \
       "${CAPACITY_EVIDENCE_SHA256_BEFORE}" &&
       "${PLAN_CAPACITY_EVIDENCE_SHA256}" == \
       "${CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE}" ]] ||
      preflight_fail "custom capacity profile identity differs from retained manifest"
    CAPACITY_EVIDENCE_POLICY=canonical_manifest_v1
  else
    [[ "${PLAN_CAPACITY_EVIDENCE_SCHEMA}" == \
       astra_pinned_trace_capacity_evidence_v1 &&
       "${PLAN_CAPACITY_PROFILE_NAME}" == \
       nasdaq-itch-20190130-acceptance-v1 &&
       "${PLAN_CAPACITY_EVIDENCE_SHA256}" == \
       "${TRACE_SHA256_BEFORE}" &&
       "${PLAN_CAPACITY_CORPUS_MANIFEST_SHA256}" == \
       "${TRACE_SHA256_BEFORE}" ]] ||
      preflight_fail "built-in acceptance capacity profile differs from the pinned trace"
    CAPACITY_EVIDENCE_POLICY=pinned_trace_builtin_v1
  fi
else
  [[ -z "${CAPACITY_EVIDENCE_FILE}" ]] ||
    preflight_fail "branch5 native acceptance does not accept a redesign capacity manifest"
  CAPACITY_EVIDENCE_POLICY=not_applicable_branch5_native_v1
fi
if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 &&
      -z "${PLANNED_BYTES_OVERRIDE}" ]]; then
  preflight_fail "branch5 native per-book capacity is unresolved before the R/S prelude; supply a conservative --planned-bytes admission bound"
fi
if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]] &&
   ! command -v python3 >/dev/null 2>&1; then
  preflight_fail "python3 is required to validate branch5 native-range manifests"
fi
if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
  for required_flag in --native-range-manifest --default-order-capacity \
                       --price-node-capacity --price-leaf-capacity \
                       --price-level-capacity; do
    case "${benchmark_help}" in
      *"${required_flag}"*) ;;
      *) preflight_fail "branch5 replay binary help is missing ${required_flag}" ;;
    esac
  done
fi

for plan_key in system_page_bytes prefault; do
  if ! read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
       "${plan_key}"; then
    preflight_fail "storage plan lacks one unambiguous ${plan_key} field"
  fi
  if ! is_uint "${RECORD_FIELD_VALUE}"; then
    preflight_fail "storage plan ${plan_key} is not numeric"
  fi
done

read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
  system_page_bytes
PLAN_SYSTEM_PAGE_BYTES="$(normalize_uint "${RECORD_FIELD_VALUE}")"
if [[ "${PLAN_SYSTEM_PAGE_BYTES}" == 0 ]] ||
   uint_gt "${PLAN_SYSTEM_PAGE_BYTES}" 1048576 ||
   [[ $((PLAN_SYSTEM_PAGE_BYTES & (PLAN_SYSTEM_PAGE_BYTES - 1))) -ne 0 ]]; then
  preflight_fail "storage-plan system page size is invalid"
fi
read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan prefault
[[ "${RECORD_FIELD_VALUE}" == 1 ]] ||
  preflight_fail "storage plan did not enable prefault"

PLAN_ARENA_MAPPED_BYTES=()
PLAN_ARENA_SUM_BYTES=0
PLAN_CORE_ARENA_SUM_BYTES=0
PLAN_MAPPED_ARRAY_BYTES=""
PLAN_DIRECT_ORDERS_MAPPED_BYTES=""
PLAN_PRICE_PAGES_MAPPED_BYTES=""
PLAN_DESCRIPTOR_BYTES=""
PLAN_DIRECT_ORDER_SLOTS=""
PLAN_FALLBACK_BUCKETS=""
PLAN_PRICE_PAGE_CAPACITY=""
PLAN_BRANCH5_PRICE_POOL_BYTES=()
PLAN_DEFAULT_ORDER_CAPACITY=""
PLAN_PRICE_INTERNAL_NODE_CAPACITY=""
PLAN_PRICE_LEAF_CAPACITY=""
PLAN_PRICE_LEVEL_CAPACITY=""

case "${HOT_ARENA_POLICY}" in
  redesign_exact_v1)
    for plan_key in mapped_array_bytes direct_orders_mapped_bytes \
                    descriptor_bytes planned_storage_bytes \
                    direct_order_slots fallback_buckets \
                    price_page_capacity; do
      if ! read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
           "${plan_key}" || ! is_uint "${RECORD_FIELD_VALUE}"; then
        preflight_fail "storage plan lacks one numeric ${plan_key} field"
      fi
    done
    if [[ $((ARENA_ALIGNMENT_BYTES % PLAN_SYSTEM_PAGE_BYTES)) -ne 0 ]]; then
      preflight_fail "2 MiB arena alignment is not divisible by the system page size"
    fi
    for arena_index in "${!ARENA_IDS[@]}"; do
      arena_id="${ARENA_IDS[arena_index]}"
      plan_key="${arena_id}_mapped_bytes"
      if ! read_record_field "${PLAN_STDOUT}" \
           itch_book_replay_storage_plan "${plan_key}" ||
         ! is_uint "${RECORD_FIELD_VALUE}"; then
        preflight_fail "storage plan lacks one numeric ${plan_key} field"
      fi
      arena_mapped_bytes="$(normalize_uint "${RECORD_FIELD_VALUE}")"
      if [[ "${arena_mapped_bytes}" == 0 ]] ||
         uint_gt "${arena_mapped_bytes}" 9223372036854775807 ||
         [[ $((arena_mapped_bytes % ARENA_ALIGNMENT_BYTES)) -ne 0 ]]; then
        preflight_fail "storage plan ${plan_key} is not a positive 2 MiB multiple"
      fi
      PLAN_ARENA_MAPPED_BYTES+=("${arena_mapped_bytes}")
      PLAN_ARENA_SUM_BYTES="$(uint_add "${PLAN_ARENA_SUM_BYTES}" \
                                      "${arena_mapped_bytes}")"
      if [[ "${ARENA_MAPPED_SUBTOTAL_FLAGS[arena_index]}" == 1 ]]; then
        PLAN_CORE_ARENA_SUM_BYTES="$(uint_add \
          "${PLAN_CORE_ARENA_SUM_BYTES}" "${arena_mapped_bytes}")"
      fi
    done
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      planned_storage_bytes
    DERIVED_PLANNED_BYTES="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      mapped_array_bytes
    PLAN_MAPPED_ARRAY_BYTES="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    if [[ "${PLAN_CORE_ARENA_SUM_BYTES}" != \
          "${PLAN_MAPPED_ARRAY_BYTES}" ]]; then
      preflight_fail "storage-plan mapped bytes differ from the ten core arena extents"
    fi
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      direct_orders_mapped_bytes
    PLAN_DIRECT_ORDERS_MAPPED_BYTES="$(normalize_uint \
      "${RECORD_FIELD_VALUE}")"
    if [[ "${PLAN_DIRECT_ORDERS_MAPPED_BYTES}" != \
          "${PLAN_ARENA_MAPPED_BYTES[ARENA_DIRECT_INDEX]}" ]]; then
      preflight_fail "legacy direct-order plan field differs from order_direct"
    fi
    PLAN_PRICE_PAGES_MAPPED_BYTES="${PLAN_ARENA_MAPPED_BYTES[ARENA_PRICE_PAGES_INDEX]}"
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      descriptor_bytes
    PLAN_DESCRIPTOR_BYTES="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    PLAN_COMPONENT_BYTES="$(uint_add "${PLAN_MAPPED_ARRAY_BYTES}" \
      "${PLAN_ARENA_MAPPED_BYTES[ARENA_DESCRIPTOR_INDEX]}")"
    if [[ "${PLAN_COMPONENT_BYTES}" != "${DERIVED_PLANNED_BYTES}" ]]; then
      preflight_fail "planned storage bytes differ from core mappings plus descriptor mapping"
    fi
    if [[ "${PLAN_ARENA_SUM_BYTES}" != "${DERIVED_PLANNED_BYTES}" ]]; then
      preflight_fail "planned storage bytes differ from all eleven arena extents"
    fi
    if [[ "${PLAN_DESCRIPTOR_BYTES}" == 0 ]] ||
       uint_gt "${PLAN_DESCRIPTOR_BYTES}" \
         "${PLAN_ARENA_MAPPED_BYTES[ARENA_DESCRIPTOR_INDEX]}"; then
      preflight_fail "logical descriptor payload exceeds its mapped arena"
    fi
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      direct_order_slots
    PLAN_DIRECT_ORDER_SLOTS="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      fallback_buckets
    PLAN_FALLBACK_BUCKETS="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      price_page_capacity
    PLAN_PRICE_PAGE_CAPACITY="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    ;;
  branch5_native_ranges_v1)
    BRANCH5_PRICE_PLAN_KEYS=(
      price_nodes_bytes price_leaves_bytes price_levels_bytes
      price_free_nodes_bytes price_free_leaves_bytes price_free_levels_bytes
    )
    for plan_key in "${BRANCH5_PRICE_PLAN_KEYS[@]}" \
                    planned_price_pool_bytes default_order_capacity \
                    price_internal_node_capacity price_leaf_capacity \
                    price_level_capacity; do
      if ! read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
           "${plan_key}" || ! is_uint "${RECORD_FIELD_VALUE}"; then
        preflight_fail "branch5 storage plan lacks one numeric ${plan_key} field"
      fi
    done
    branch5_price_sum=0
    for plan_key in "${BRANCH5_PRICE_PLAN_KEYS[@]}"; do
      read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
        "${plan_key}"
      plan_value="$(normalize_uint "${RECORD_FIELD_VALUE}")"
      if [[ "${plan_value}" == 0 ]] ||
         uint_gt "${plan_value}" 9223372036854775807; then
        preflight_fail "branch5 storage-plan ${plan_key} is invalid"
      fi
      PLAN_BRANCH5_PRICE_POOL_BYTES+=("${plan_value}")
      branch5_price_sum="$(uint_add "${branch5_price_sum}" "${plan_value}")"
    done
    read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
      planned_price_pool_bytes
    DERIVED_PLANNED_BYTES="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    if [[ "${branch5_price_sum}" != "${DERIVED_PLANNED_BYTES}" ]]; then
      preflight_fail "branch5 price-pool plan differs from its six vector byte sizes"
    fi
    for plan_key in default_order_capacity price_internal_node_capacity \
                    price_leaf_capacity price_level_capacity; do
      read_record_field "${PLAN_STDOUT}" itch_book_replay_storage_plan \
        "${plan_key}"
      plan_value="$(normalize_uint "${RECORD_FIELD_VALUE}")"
      [[ "${plan_value}" != 0 ]] ||
        preflight_fail "branch5 storage-plan ${plan_key} is zero"
      case "${plan_key}" in
        default_order_capacity) PLAN_DEFAULT_ORDER_CAPACITY="${plan_value}" ;;
        price_internal_node_capacity)
          PLAN_PRICE_INTERNAL_NODE_CAPACITY="${plan_value}" ;;
        price_leaf_capacity) PLAN_PRICE_LEAF_CAPACITY="${plan_value}" ;;
        price_level_capacity) PLAN_PRICE_LEVEL_CAPACITY="${plan_value}" ;;
      esac
    done
    if ! branch5_plan_is_canonical \
         "${PLAN_DEFAULT_ORDER_CAPACITY}" \
         "${PLAN_PRICE_INTERNAL_NODE_CAPACITY}" \
         "${PLAN_PRICE_LEAF_CAPACITY}" \
         "${PLAN_PRICE_LEVEL_CAPACITY}" \
         "${DERIVED_PLANNED_BYTES}"; then
      preflight_fail "branch5 storage plan differs from the pinned native capacities"
    fi
    ;;
esac

if [[ "${HOT_ARENA_POLICY}" == redesign_exact_v1 ]]; then
  profiled_direct_extent="$(uint_add \
    "${PLAN_CAPACITY_PROFILED_MAX_ORDER_REF}" 1)"
  if uint_gt "${profiled_direct_extent}" "${PLAN_DIRECT_ORDER_SLOTS}"; then
    preflight_fail "profiled maximum order reference exceeds the planned direct table"
  fi
  computed_direct_headroom="$(uint_subtract \
    "${PLAN_DIRECT_ORDER_SLOTS}" "${profiled_direct_extent}")" ||
    preflight_fail "cannot compute direct-order profile headroom"
  [[ "${computed_direct_headroom}" == \
     "$(normalize_uint "${PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM}")" ]] ||
    preflight_fail "capacity profile direct headroom differs from configured capacity"
  uint_ge "${computed_direct_headroom}" \
      "${PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM}" ||
    preflight_fail "capacity profile direct headroom is below its minimum"

  if uint_gt "${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}" \
       "${PLAN_PRICE_PAGE_CAPACITY}" ||
     [[ "$(normalize_uint "${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}")" == \
        "${PLAN_PRICE_PAGE_CAPACITY}" ]]; then
    preflight_fail "profiled unique price pages leave no planned page headroom"
  fi
  computed_price_headroom="$(uint_subtract \
    "${PLAN_PRICE_PAGE_CAPACITY}" \
    "${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}")" ||
    preflight_fail "cannot compute price-page profile headroom"
  [[ "${computed_price_headroom}" == \
     "$(normalize_uint "${PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM}")" ]] ||
    preflight_fail "capacity profile price headroom differs from configured capacity"
  uint_ge "${computed_price_headroom}" \
      "${PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM}" ||
    preflight_fail "capacity profile price headroom is below its minimum"

  if [[ "${CAPACITY_EVIDENCE_POLICY}" == pinned_trace_builtin_v1 ]]; then
    [[ "${PLAN_CAPACITY_PROFILER_SHA256}" == \
       c7f468bd3bc784398626997329f01653ac54b8691af822419931f23e95907956 &&
       "$(normalize_uint "${PLAN_CAPACITY_PROFILED_MAX_ORDER_REF}")" == \
       329176641 &&
       "$(normalize_uint "${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}")" == \
       68941 &&
       "$(normalize_uint "${PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM}")" == \
       207694270 &&
       "$(normalize_uint "${PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM}")" == \
       207694270 &&
       "$(normalize_uint "${PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM}")" == \
       11059 &&
       "$(normalize_uint "${PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM}")" == \
       11059 &&
       "${PLAN_DIRECT_ORDER_SLOTS}" == 536870912 &&
       "${PLAN_FALLBACK_BUCKETS}" == 1048576 &&
       "${PLAN_PRICE_PAGE_CAPACITY}" == 80000 ]] ||
      preflight_fail "built-in capacity fields differ from the reviewed pinned profile"
  fi
fi

if [[ "${DERIVED_PLANNED_BYTES}" == 0 ]] ||
   uint_gt "${DERIVED_PLANNED_BYTES}" 9223372036854775807; then
  preflight_fail "derived planned storage bytes are invalid"
fi

if [[ -n "${PLANNED_BYTES_OVERRIDE}" ]]; then
  if ! uint_ge "${PLANNED_BYTES_OVERRIDE}" "${DERIVED_PLANNED_BYTES}"; then
    preflight_fail "--planned-bytes is below the binary-derived storage plan"
  fi
  PLANNED_BYTES="${PLANNED_BYTES_OVERRIDE}"
else
  PLANNED_BYTES="${DERIVED_PLANNED_BYTES}"
fi
REQUIRED_BYTES="$(uint_add "${PLANNED_BYTES}" "${RESERVE_BYTES}")"
if uint_gt "${REQUIRED_BYTES}" "${UINT64_MAX_VALUE}"; then
  preflight_fail "planned footprint plus reserve exceeds uint64"
fi

read_kb_field() {
  local file="$1"
  local field="$2"
  awk -v wanted="${field}:" '
    {
      for (field_number = 1; field_number < NF; ++field_number) {
        if ($field_number == wanted) {
          print $(field_number + 1)
          found = 1
          exit
        }
      }
    }
    END { if (!found) exit 1 }
  ' "${file}"
}

NODE_DIR="/sys/devices/system/node/node${NUMA_NODE}"
[[ -d "${NODE_DIR}" ]] || preflight_fail "NUMA node ${NUMA_NODE} is absent"
[[ -e "${NODE_DIR}/cpu${CPU}" ]] ||
  preflight_fail "CPU ${CPU} is not part of NUMA node ${NUMA_NODE}"
[[ -d "/sys/devices/system/cpu/cpu${MONITOR_CPU}" ]] ||
  preflight_fail "monitor CPU ${MONITOR_CPU} is absent"
[[ -r "${NODE_DIR}/meminfo" ]] || preflight_fail "node meminfo is unreadable"

NODE_MEM_TOTAL_KB="$(read_kb_field "${NODE_DIR}/meminfo" MemTotal)" ||
  preflight_fail "cannot read node MemTotal"
NODE_MEM_FREE_KB="$(read_kb_field "${NODE_DIR}/meminfo" MemFree)" ||
  preflight_fail "cannot read node MemFree"
NODE_MEM_TOTAL_BYTES="$(uint_multiply_small "${NODE_MEM_TOTAL_KB}" 1024)"
NODE_MEM_FREE_BYTES="$(uint_multiply_small "${NODE_MEM_FREE_KB}" 1024)"

[[ -r /proc/sys/kernel/random/boot_id ]] ||
  preflight_fail "kernel boot ID is unreadable"
BOOT_ID="$(</proc/sys/kernel/random/boot_id)"
[[ "${BOOT_ID}" =~ ^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$ ]] ||
  preflight_fail "kernel boot ID is invalid"
MACHINE="$(uname -m)" || preflight_fail "cannot read machine architecture"
[[ -r /sys/devices/system/cpu/online ]] ||
  preflight_fail "online CPU list is unreadable"
CPU_ONLINE_LIST="$(</sys/devices/system/cpu/online)"
[[ -n "${CPU_ONLINE_LIST}" ]] || preflight_fail "online CPU list is empty"
CPU_ISOLATED_LIST=unavailable
if [[ -r /sys/devices/system/cpu/isolated ]]; then
  CPU_ISOLATED_LIST="$(</sys/devices/system/cpu/isolated)"
  [[ -n "${CPU_ISOLATED_LIST}" ]] || CPU_ISOLATED_LIST=none
fi
CPU_LIST_VALID=0
CPU_LIST_MATCH=0
parse_cpu_list_membership() {
  local list="$1"
  local target="$2"
  local range=""
  local start=""
  local end=""
  local old_ifs="${IFS}"
  local ranges=()

  CPU_LIST_VALID=0
  CPU_LIST_MATCH=0
  [[ "${list}" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] || return 1
  IFS=,
  read -r -a ranges <<<"${list}"
  IFS="${old_ifs}"
  for range in "${ranges[@]}"; do
    start="${range%%-*}"
    if [[ "${range}" == *-* ]]; then
      end="${range#*-}"
    else
      end="${start}"
    fi
    start="$(normalize_uint "${start}")"
    end="$(normalize_uint "${end}")"
    if uint_gt "${start}" "${end}"; then
      return 1
    fi
    if uint_ge "${target}" "${start}" && uint_ge "${end}" "${target}"; then
      CPU_LIST_MATCH=1
    fi
  done
  CPU_LIST_VALID=1
  return 0
}
if ! parse_cpu_list_membership "${CPU_ONLINE_LIST}" "${CPU}" ||
   [[ "${CPU_LIST_VALID}" -ne 1 || "${CPU_LIST_MATCH}" -ne 1 ]]; then
  preflight_fail "candidate CPU is absent from the online CPU list"
fi
if [[ "${CPU_ISOLATED_LIST}" == unavailable ||
      "${CPU_ISOLATED_LIST}" == none ]] ||
   ! parse_cpu_list_membership "${CPU_ISOLATED_LIST}" "${CPU}" ||
   [[ "${CPU_LIST_VALID}" -ne 1 || "${CPU_LIST_MATCH}" -ne 1 ]]; then
  preflight_fail "candidate CPU is not domain-isolated in /sys/devices/system/cpu/isolated"
fi
CPU_SCALING_GOVERNOR=unavailable
CPU_SCALING_DRIVER=unavailable
CPUFREQ_DIR="/sys/devices/system/cpu/cpu${CPU}/cpufreq"
if [[ -r "${CPUFREQ_DIR}/scaling_governor" ]]; then
  CPU_SCALING_GOVERNOR="$(<"${CPUFREQ_DIR}/scaling_governor")"
  [[ "${CPU_SCALING_GOVERNOR}" == performance ]] ||
    preflight_fail "candidate CPU scaling governor must be performance when exposed"
fi
if [[ -r "${CPUFREQ_DIR}/scaling_driver" ]]; then
  CPU_SCALING_DRIVER="$(<"${CPUFREQ_DIR}/scaling_driver")"
fi
cpuinfo_value() {
  local key="$1"
  awk -F: -v selected_cpu="${CPU}" -v wanted="${key}" '
    {
      field = $1
      value = substr($0, index($0, ":") + 1)
      sub(/^[[:space:]]+/, "", field)
      sub(/[[:space:]]+$/, "", field)
      sub(/^[[:space:]]+/, "", value)
      sub(/[[:space:]]+$/, "", value)
      if (field == "processor") {
        selected = (value == selected_cpu)
        next
      }
      if (selected && field == wanted) {
        print value
        found = 1
        exit
      }
    }
    END { if (!found) exit 1 }
  ' /proc/cpuinfo
}
CPU_VENDOR_ID="$(cpuinfo_value vendor_id 2>/dev/null || true)"
CPU_FAMILY="$(cpuinfo_value 'cpu family' 2>/dev/null || true)"
CPU_MODEL="$(cpuinfo_value model 2>/dev/null || true)"
CPU_STEPPING="$(cpuinfo_value stepping 2>/dev/null || true)"
CPU_MODEL_NAME="$(cpuinfo_value 'model name' 2>/dev/null || true)"
[[ -n "${CPU_VENDOR_ID}" && -n "${CPU_FAMILY}" && -n "${CPU_MODEL}" &&
   -n "${CPU_STEPPING}" && -n "${CPU_MODEL_NAME}" ]] ||
  preflight_fail "selected CPU identity is incomplete in /proc/cpuinfo"

{
  echo "artifact_schema=astra_order_book_acceptance_v1"
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host=$(hostname)"
  echo "kernel=$(uname -srvm)"
  echo "boot_id=${BOOT_ID}"
  echo "machine=${MACHINE}"
  echo "cpu_vendor_id=${CPU_VENDOR_ID}"
  echo "cpu_family=${CPU_FAMILY}"
  echo "cpu_model=${CPU_MODEL}"
  echo "cpu_stepping=${CPU_STEPPING}"
  echo "cpu_model_name=${CPU_MODEL_NAME}"
  echo "cpu_online_list=${CPU_ONLINE_LIST}"
  echo "cpu_isolated_list=${CPU_ISOLATED_LIST}"
  echo "cpu_isolation_source=sysfs_domain_isolated"
  echo "cpu_scaling_governor=${CPU_SCALING_GOVERNOR}"
  echo "cpu_scaling_driver=${CPU_SCALING_DRIVER}"
  printf 'binary_requested='; printf '%q\n' "${BINARY_REQUESTED}"
  echo "binary=${BINARY}"
  echo "binary_sha256=${BINARY_SHA256}"
  echo "binary_size_bytes=${BINARY_SIZE_BYTES}"
  echo "binary_archive=${BINARY_ARCHIVE}"
  echo "binary_archive_sha256=${BINARY_ARCHIVE_SHA256_BEFORE}"
  echo "source_build_attestation_version=1"
  echo "source_build_mode=git_archive_fresh_cmake_clean_first_v1"
  echo "source_build_target=${SOURCE_BUILD_TARGET}"
  echo "source_build_attestation_sha256=${SOURCE_BUILD_ATTESTATION_SHA256_BEFORE}"
  echo "source_tree_archive_sha256=${SOURCE_TREE_ARCHIVE_SHA256}"
  echo "fresh_binary_sha256=${SOURCE_BUILD_BINARY_SHA256}"
  echo "fresh_binary_archive_sha256=${SOURCE_BUILD_BINARY_ARCHIVE_SHA256_BEFORE}"
  printf 'trace_requested='; printf '%q\n' "${TRACE_REQUESTED}"
  echo "trace=${TRACE}"
  echo "trace_size_bytes=${TRACE_SIZE_BYTES}"
  echo "trace_sha256=${TRACE_SHA256}"
  echo "trace_hash_policy=mandatory_pre_and_post"
  echo "hash_trace_option_deprecated_noop=${HASH_TRACE}"
  echo "capacity_profile_policy=${CAPACITY_EVIDENCE_POLICY}"
  echo "capacity_profile_bound=${PLAN_CAPACITY_PROFILE_BOUND}"
  echo "capacity_evidence_schema=${PLAN_CAPACITY_EVIDENCE_SCHEMA}"
  echo "capacity_profile_name=${PLAN_CAPACITY_PROFILE_NAME}"
  echo "capacity_evidence_sha256=${PLAN_CAPACITY_EVIDENCE_SHA256}"
  echo "capacity_corpus_manifest_sha256=${PLAN_CAPACITY_CORPUS_MANIFEST_SHA256}"
  echo "capacity_profiler_sha256=${PLAN_CAPACITY_PROFILER_SHA256}"
  echo "capacity_evidence_source_sha256=${CAPACITY_EVIDENCE_SHA256_BEFORE}"
  echo "capacity_evidence_archive_sha256=${CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE}"
  echo "capacity_profiled_max_order_ref=${PLAN_CAPACITY_PROFILED_MAX_ORDER_REF}"
  echo "capacity_profiled_unique_price_pages=${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}"
  echo "capacity_minimum_direct_order_headroom=${PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM}"
  echo "capacity_effective_direct_order_headroom=${PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM}"
  echo "capacity_minimum_price_page_headroom=${PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM}"
  echo "capacity_effective_price_page_headroom=${PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM}"
  echo "harness_source=${HARNESS_SOURCE}"
  echo "harness_source_sha256=${HARNESS_SHA256_BEFORE}"
  echo "harness_archive=${HARNESS_ARCHIVE}"
  echo "harness_archive_sha256=${HARNESS_ARCHIVE_SHA256_BEFORE}"
  echo "hot_path_verifier_source=${HOT_PATH_VERIFIER_SOURCE}"
  echo "hot_path_verifier_source_sha256=${HOT_PATH_VERIFIER_SHA256_BEFORE}"
  echo "hot_path_verifier_archive=${HOT_PATH_VERIFIER_ARCHIVE}"
  echo "hot_path_verifier_archive_sha256=${HOT_PATH_VERIFIER_ARCHIVE_SHA256_BEFORE}"
  echo "hot_path_verifier_stdout=${HOT_PATH_VERIFIER_STDOUT}"
  echo "hot_path_verifier_report=${HOT_PATH_VERIFIER_REPORT}"
  echo "git_dirty=${GIT_DIRTY}"
  echo "git_status_file=${OUTPUT_DIR}/git-status-porcelain.txt"
  echo "git_commit_before=${GIT_COMMIT_BEFORE}"
  echo "git_parent_before=${GIT_PARENT_BEFORE}"
  echo "git_tree_before=${GIT_TREE_BEFORE}"
  echo "git_branch_before=${GIT_BRANCH_BEFORE}"
  echo "git_fingerprint_before=${GIT_FINGERPRINT_BEFORE}"
  echo "git_artifact_path_excluded=${OUTPUT_REPOSITORY_PATH}"
  echo "benchmark_help_file=${OUTPUT_DIR}/benchmark-help.txt"
  echo "cmake_build_directory=${CMAKE_BUILD_DIRECTORY}"
  echo "cmake_cache=${CMAKE_CACHE}"
  echo "cmake_cache_sha256=${CMAKE_CACHE_SHA256_BEFORE}"
  echo "cmake_cache_archive=${CMAKE_CACHE_ARCHIVE}"
  echo "cmake_build_type=${CMAKE_BUILD_TYPE}"
  echo "cmake_generator=${CMAKE_GENERATOR}"
  echo "cmake_make_program=${CMAKE_MAKE_PROGRAM}"
  echo "cmake_cxx_compiler=${CMAKE_CXX_COMPILER}"
  echo "cmake_cxx_compiler_id=${CMAKE_CXX_COMPILER_ID}"
  echo "cmake_cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION}"
  echo "compiler_version_file=${COMPILER_VERSION_FILE}"
  echo "target_build_commands_file=${TARGET_BUILD_COMMANDS_FILE}"
  echo "artifact_manifest=${OUTPUT_DIR}/manifest.sha256"
  echo "runs=${RUNS}"
  echo "cpu=${CPU}"
  echo "monitor_cpu=${MONITOR_CPU}"
  echo "numa_node=${NUMA_NODE}"
  echo "node_cpulist=$(<"${NODE_DIR}/cpulist")"
  echo "node_mem_total_bytes=${NODE_MEM_TOTAL_BYTES}"
  echo "node_mem_free_bytes=${NODE_MEM_FREE_BYTES}"
  echo "derived_planned_storage_bytes=${DERIVED_PLANNED_BYTES}"
  if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
    echo "admission_basis=explicit_override_plus_reserve"
    echo "branch5_capacity_policy=${BRANCH5_CAPACITY_POLICY}"
    echo "branch5_capacity_profile_name=${BRANCH5_CAPACITY_PROFILE_NAME}"
    echo "branch5_capacity_evidence_policy=${BRANCH5_CAPACITY_EVIDENCE_POLICY}"
    echo "branch5_runtime_capacity_mode=${BRANCH5_RUNTIME_CAPACITY_MODE}"
    echo "branch5_plan_scope=shared_price_pool_only"
    echo "branch5_per_book_bytes_resolved_at_ready=1"
  else
    echo "admission_basis=complete_redesign_arena_plan_plus_reserve"
  fi
  echo "planned_bytes=${PLANNED_BYTES}"
  echo "reserve_bytes=${RESERVE_BYTES}"
  echo "required_bytes=${REQUIRED_BYTES}"
  echo "expected_records=${EXPECTED_RECORDS}"
  echo "expected_bytes=${EXPECTED_BYTES}"
  echo "expected_mutation_digest=${EXPECTED_MUTATION_DIGEST:-discovered_or_not_requested}"
  echo "expected_semantic_mutation_digest=${EXPECTED_SEMANTIC_MUTATION_DIGEST:-discovered_or_not_requested}"
  echo "max_p50_ns=${MAX_P50_NS}"
  echo "max_p99_ns=${MAX_P99_NS}"
  echo "max_p99_9_ns=${MAX_P999_NS}"
  echo "sample_every=${SAMPLE_EVERY}"
  echo "warmup_book_messages=${WARMUP_BOOK_MESSAGES}"
  echo "min_samples=${MIN_SAMPLES}"
  echo "plan_hot_arena_schema=${PLAN_HOT_ARENA_SCHEMA}"
  echo "plan_hot_arena_policy=${HOT_ARENA_POLICY}"
  echo "plan_hot_arena_count=${#ARENA_IDS[@]}"
  echo "plan_hot_arena_alignment_bytes=${ARENA_ALIGNMENT_BYTES}"
  echo "plan_system_page_bytes=${PLAN_SYSTEM_PAGE_BYTES}"
  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      echo "plan_mapped_array_bytes=${PLAN_MAPPED_ARRAY_BYTES}"
      echo "plan_direct_orders_mapped_bytes=${PLAN_DIRECT_ORDERS_MAPPED_BYTES}"
      echo "plan_price_pages_mapped_bytes=${PLAN_PRICE_PAGES_MAPPED_BYTES}"
      for arena_index in "${!ARENA_IDS[@]}"; do
        echo "plan_${ARENA_IDS[arena_index]}_mapped_bytes=${PLAN_ARENA_MAPPED_BYTES[arena_index]}"
        echo "plan_${ARENA_IDS[arena_index]}_vma_name=${ARENA_VMA_NAMES[arena_index]}"
      done
      echo "plan_descriptor_bytes=${PLAN_DESCRIPTOR_BYTES}"
      echo "plan_direct_order_slots=${PLAN_DIRECT_ORDER_SLOTS}"
      echo "plan_fallback_buckets=${PLAN_FALLBACK_BUCKETS}"
      echo "plan_price_page_capacity=${PLAN_PRICE_PAGE_CAPACITY}"
      ;;
    branch5_native_ranges_v1)
      echo "plan_price_pool_bytes=${DERIVED_PLANNED_BYTES}"
      for arena_index in "${!BRANCH5_PRICE_PLAN_KEYS[@]}"; do
        echo "plan_${BRANCH5_PRICE_PLAN_KEYS[arena_index]}=${PLAN_BRANCH5_PRICE_POOL_BYTES[arena_index]}"
      done
      echo "plan_default_order_capacity=${PLAN_DEFAULT_ORDER_CAPACITY}"
      echo "plan_price_internal_node_capacity=${PLAN_PRICE_INTERNAL_NODE_CAPACITY}"
      echo "plan_price_leaf_capacity=${PLAN_PRICE_LEAF_CAPACITY}"
      echo "plan_price_level_capacity=${PLAN_PRICE_LEVEL_CAPACITY}"
      ;;
  esac
  echo "sample_capacity_override=${SAMPLE_CAPACITY:-binary-default}"
  echo "allow_swap=${ALLOW_SWAP}"
  echo "run_perf_stat=${RUN_PERF_STAT}"
  echo "proc_self_binding:"
  awk '/^(Cpus_allowed_list|Mems_allowed_list):/ { print }' /proc/self/status
  echo "numactl_show:"
  if ! "${NUMACTL_BIN}" --show 2>&1; then
    echo "numactl_show_status=failed"
  fi
  echo "numactl_hardware:"
  if ! "${NUMACTL_BIN}" --hardware 2>&1; then
    echo "numactl_hardware_status=failed"
  fi
  if command -v lscpu >/dev/null 2>&1; then
    echo "lscpu:"
    if ! lscpu; then
      echo "lscpu_status=failed"
    fi
  fi
} >"${PREFLIGHT_FILE}"

if ! uint_ge "${NODE_MEM_TOTAL_BYTES}" "${REQUIRED_BYTES}"; then
  preflight_fail "node MemTotal is below planned bytes plus reserve"
fi
if ! uint_ge "${NODE_MEM_FREE_BYTES}" "${REQUIRED_BYTES}"; then
  preflight_fail "node MemFree is below planned bytes plus reserve"
fi

if ! "${NUMACTL_BIN}" "--physcpubind=${CPU}" "--membind=${NUMA_NODE}" true; then
  preflight_fail "numactl cannot establish the requested CPU/node binding"
fi
if ! command -v taskset >/dev/null 2>&1; then
  preflight_fail "taskset is required to keep the memory monitor off the candidate CPU"
fi
if ! taskset -c "${MONITOR_CPU}" true; then
  preflight_fail "taskset cannot use monitor CPU ${MONITOR_CPU}"
fi
PERF_BIN=perf
if [[ "${RUN_PERF_STAT}" -eq 1 ]]; then
  if ! command -v perf >/dev/null 2>&1; then
    preflight_fail "perf is required unless --no-perf-stat is explicit"
  fi
  PERF_BIN="$(command -v perf)"
  if PERF_VERSION="$("${PERF_BIN}" --version 2>&1)"; then
    echo "perf_version=${PERF_VERSION}" >>"${PREFLIGHT_FILE}"
  else
    preflight_fail "perf --version failed"
  fi
  PERF_PREFLIGHT_PREFIX="${OUTPUT_DIR}/perf-preflight"
  PERF_PREFLIGHT_COUNTERS="${PERF_PREFLIGHT_PREFIX}.perf-stat.csv"
  PERF_PREFLIGHT_COMMAND=(
    "${PERF_BIN}" stat
    -x ';'
    -o "${PERF_PREFLIGHT_COUNTERS}"
    -e "${PERF_EVENTS}"
    --
    "${NUMACTL_BIN}" "--physcpubind=${CPU}" "--membind=${NUMA_NODE}" \
    sleep 1
  )
  write_command_file "${PERF_PREFLIGHT_PREFIX}.command.txt" \
    "${PERF_PREFLIGHT_COMMAND[@]}"
  if ! "${PERF_PREFLIGHT_COMMAND[@]}" \
       >"${PERF_PREFLIGHT_PREFIX}.stdout.log" \
       2>"${PERF_PREFLIGHT_PREFIX}.stderr.log"; then
    preflight_fail "perf cannot count the required event set on CPU ${CPU}"
  fi
  if ! validate_perf_counters "${PERF_PREFLIGHT_COUNTERS}"; then
    preflight_fail "${PERF_VALIDATION_ERROR}"
  fi
fi

SWAP_TOTAL_KB="$(read_kb_field /proc/meminfo SwapTotal)" ||
  preflight_fail "cannot read SwapTotal"
SWAP_FREE_KB="$(read_kb_field /proc/meminfo SwapFree)" ||
  preflight_fail "cannot read SwapFree"
COMMIT_LIMIT_KB="$(read_kb_field /proc/meminfo CommitLimit)" ||
  preflight_fail "cannot read CommitLimit"
COMMITTED_AS_KB="$(read_kb_field /proc/meminfo Committed_AS)" ||
  preflight_fail "cannot read Committed_AS"
OVERCOMMIT_MEMORY="$(</proc/sys/vm/overcommit_memory)"
OVERCOMMIT_RATIO="$(</proc/sys/vm/overcommit_ratio)"
THP_ENABLED_STATE=unavailable
THP_DEFRAG_STATE=unavailable
if [[ -r /sys/kernel/mm/transparent_hugepage/enabled ]]; then
  THP_ENABLED_STATE="$(</sys/kernel/mm/transparent_hugepage/enabled)"
fi
if [[ -r /sys/kernel/mm/transparent_hugepage/defrag ]]; then
  THP_DEFRAG_STATE="$(</sys/kernel/mm/transparent_hugepage/defrag)"
fi

{
  echo "swap_total_kb=${SWAP_TOTAL_KB}"
  echo "swap_free_kb=${SWAP_FREE_KB}"
  echo "vm_overcommit_memory=${OVERCOMMIT_MEMORY}"
  echo "vm_overcommit_ratio=${OVERCOMMIT_RATIO}"
  echo "commit_limit_kb=${COMMIT_LIMIT_KB}"
  echo "committed_as_kb=${COMMITTED_AS_KB}"
  echo "thp_enabled=${THP_ENABLED_STATE}"
  echo "thp_defrag=${THP_DEFRAG_STATE}"
  echo "proc_self_cgroup:"
  cat /proc/self/cgroup
} >>"${PREFLIGHT_FILE}"

if [[ "${HOT_ARENA_POLICY}" == redesign_exact_v1 &&
      ! "${THP_ENABLED_STATE}" =~ \[(always|madvise)\] ]]; then
  preflight_fail "transparent huge pages must enable always or madvise mode"
fi

if [[ "${ALLOW_SWAP}" -eq 0 && "$(normalize_uint "${SWAP_TOTAL_KB}")" != 0 ]]; then
  preflight_fail "swap is enabled; disable it or pass --allow-swap explicitly"
fi

if [[ "${OVERCOMMIT_MEMORY}" == 2 ]]; then
  COMMIT_LIMIT_BYTES="$(uint_multiply_small "${COMMIT_LIMIT_KB}" 1024)"
  COMMITTED_AS_BYTES="$(uint_multiply_small "${COMMITTED_AS_KB}" 1024)"
  if uint_ge "${COMMITTED_AS_BYTES}" "${COMMIT_LIMIT_BYTES}"; then
    preflight_fail "strict overcommit has no commit headroom"
  fi
  COMMIT_AVAILABLE_BYTES="$(uint_subtract "${COMMIT_LIMIT_BYTES}" \
                                           "${COMMITTED_AS_BYTES}")" ||
    preflight_fail "cannot derive strict-overcommit headroom"
  echo "commit_available_bytes=${COMMIT_AVAILABLE_BYTES}" >>"${PREFLIGHT_FILE}"
  if ! uint_ge "${COMMIT_AVAILABLE_BYTES}" "${REQUIRED_BYTES}"; then
    preflight_fail "strict-overcommit commit headroom is below required bytes"
  fi
fi

CGROUP_MEMORY_VERSION=unavailable
CGROUP_MEMORY_DIR=""
CGROUP_MEMORY_ROOT=""
CGROUP_LIMIT_INDEX=0
CGROUP_FINITE_LIMITS=0
CGROUP_EFFECTIVE_MAX=unavailable
CGROUP_EFFECTIVE_AVAILABLE=unavailable

check_cgroup_directory() {
  local directory="$1"
  local version="$2"
  local max_name=""
  local current_name=""
  local maximum=""
  local current="unavailable"
  local available="unavailable"
  local status="finite"

  if [[ "${version}" == v2 ]]; then
    max_name=memory.max
    current_name=memory.current
  else
    max_name=memory.limit_in_bytes
    current_name=memory.usage_in_bytes
  fi
  if [[ ! -r "${directory}/${max_name}" ]]; then
    preflight_fail "cannot read ${max_name} for cgroup ${directory}"
  fi
  maximum="$(<"${directory}/${max_name}")"
  if [[ -r "${directory}/${current_name}" ]]; then
    current="$(<"${directory}/${current_name}")"
  fi

  if [[ "${version}" == v2 && "${maximum}" == max ]]; then
    status=unlimited
  else
    if ! is_uint "${maximum}"; then
      preflight_fail "cgroup memory limit is not numeric at ${directory}"
    fi
    # Values above signed 64-bit are the conventional cgroup-v1 unlimited
    # sentinel. They are evidence, not a finite admission constraint.
    if [[ "${version}" == v1 ]] &&
       uint_gt "${maximum}" 9223372036854775807; then
      status=unlimited
    fi
  fi

  if [[ "${status}" == finite ]]; then
    CGROUP_FINITE_LIMITS=$((CGROUP_FINITE_LIMITS + 1))
    if [[ "${current}" == unavailable ]]; then
      preflight_fail "cannot read current memory for finite cgroup ${directory}"
    fi
    if ! is_uint "${current}"; then
      preflight_fail "cgroup current memory is not numeric at ${directory}"
    fi
    if uint_ge "${current}" "${maximum}"; then
      preflight_fail "cgroup has no memory headroom at ${directory}"
    fi
    available="$(uint_subtract "${maximum}" "${current}")" ||
      preflight_fail "cannot derive cgroup headroom at ${directory}"
    if ! uint_ge "${maximum}" "${REQUIRED_BYTES}"; then
      preflight_fail "cgroup memory limit is below required bytes at ${directory}"
    fi
    if ! uint_ge "${available}" "${REQUIRED_BYTES}"; then
      preflight_fail "cgroup memory headroom is below required bytes at ${directory}"
    fi
    if [[ "${CGROUP_EFFECTIVE_MAX}" == unavailable ]] ||
       uint_gt "${CGROUP_EFFECTIVE_MAX}" "${maximum}"; then
      CGROUP_EFFECTIVE_MAX="$(normalize_uint "${maximum}")"
    fi
    if [[ "${CGROUP_EFFECTIVE_AVAILABLE}" == unavailable ]] ||
       uint_gt "${CGROUP_EFFECTIVE_AVAILABLE}" "${available}"; then
      CGROUP_EFFECTIVE_AVAILABLE="$(normalize_uint "${available}")"
    fi
  fi

  {
    echo "cgroup_limit_${CGROUP_LIMIT_INDEX}_directory=${directory}"
    echo "cgroup_limit_${CGROUP_LIMIT_INDEX}_version=${version}"
    echo "cgroup_limit_${CGROUP_LIMIT_INDEX}_maximum=${maximum}"
    echo "cgroup_limit_${CGROUP_LIMIT_INDEX}_current=${current}"
    echo "cgroup_limit_${CGROUP_LIMIT_INDEX}_available=${available}"
    echo "cgroup_limit_${CGROUP_LIMIT_INDEX}_status=${status}"
  } >>"${PREFLIGHT_FILE}"
  CGROUP_LIMIT_INDEX=$((CGROUP_LIMIT_INDEX + 1))
}

walk_cgroup_hierarchy() {
  local directory="$1"
  local root="$2"
  local version="$3"
  local parent=""

  case "${directory}" in
    "${root}"|"${root}"/*) ;;
    *) preflight_fail "cgroup path escapes expected mount root" ;;
  esac
  while :; do
    check_cgroup_directory "${directory}" "${version}"
    if [[ "${directory}" == "${root}" ]]; then
      break
    fi
    parent="${directory%/*}"
    if [[ -z "${parent}" || "${parent}" == "${directory}" ]]; then
      preflight_fail "cannot walk cgroup hierarchy from ${directory}"
    fi
    directory="${parent}"
  done
}

CGROUP_V2_REL="$(awk -F: '$1 == "0" { print $3; found = 1 }
                         END { if (!found) exit 1 }' /proc/self/cgroup 2>/dev/null || true)"
if [[ -n "${CGROUP_V2_REL}" ]]; then
  CGROUP_MEMORY_VERSION=v2
  CGROUP_MEMORY_ROOT=/sys/fs/cgroup
  CGROUP_MEMORY_DIR="${CGROUP_MEMORY_ROOT}${CGROUP_V2_REL}"
  CGROUP_MEMORY_DIR="${CGROUP_MEMORY_DIR%/}"
  [[ -n "${CGROUP_MEMORY_DIR}" ]] || CGROUP_MEMORY_DIR="${CGROUP_MEMORY_ROOT}"
  walk_cgroup_hierarchy "${CGROUP_MEMORY_DIR}" "${CGROUP_MEMORY_ROOT}" v2
else
  CGROUP_V1_REL="$(awk -F: '$2 ~ /(^|,)memory(,|$)/ { print $3; found = 1 }
                           END { if (!found) exit 1 }' /proc/self/cgroup 2>/dev/null || true)"
  if [[ -n "${CGROUP_V1_REL}" ]]; then
    CGROUP_MEMORY_VERSION=v1
    CGROUP_MEMORY_ROOT=/sys/fs/cgroup/memory
    CGROUP_MEMORY_DIR="${CGROUP_MEMORY_ROOT}${CGROUP_V1_REL}"
    CGROUP_MEMORY_DIR="${CGROUP_MEMORY_DIR%/}"
    [[ -n "${CGROUP_MEMORY_DIR}" ]] || CGROUP_MEMORY_DIR="${CGROUP_MEMORY_ROOT}"
    walk_cgroup_hierarchy "${CGROUP_MEMORY_DIR}" "${CGROUP_MEMORY_ROOT}" v1
  fi
fi
{
  echo "cgroup_memory_version=${CGROUP_MEMORY_VERSION}"
  echo "cgroup_memory_dir=${CGROUP_MEMORY_DIR:-unavailable}"
  echo "cgroup_finite_limit_count=${CGROUP_FINITE_LIMITS}"
  echo "cgroup_effective_max=${CGROUP_EFFECTIVE_MAX}"
  echo "cgroup_effective_available=${CGROUP_EFFECTIVE_AVAILABLE}"
} >>"${PREFLIGHT_FILE}"

echo "git_commit=${GIT_COMMIT_BEFORE}" >>"${PREFLIGHT_FILE}"

cat "${PREFLIGHT_FILE}"

snapshot_numa_maps() {
  local source="$1"
  local destination="$2"
  awk -v selected="N${NUMA_NODE}" '
    {
      line_is_anon = 0
      for (field_number = 1; field_number <= NF; ++field_number) {
        if ($field_number ~ /^anon=[1-9][0-9]*$/)
          line_is_anon = 1
      }
      for (field_number = 1; field_number <= NF; ++field_number) {
        if ($field_number ~ /^N[0-9]+=[0-9]+$/) {
          split($field_number, pair, "=")
          all_pages[pair[1]] += pair[2]
          if (line_is_anon)
            anon_pages[pair[1]] += pair[2]
        }
      }
    }
    END {
      for (node in all_pages)
        print node "_all_pages=" all_pages[node]
      for (node in anon_pages) {
        print node "_anon_pages=" anon_pages[node]
        anon_total += anon_pages[node]
      }
      selected_pages = anon_pages[selected] + 0
      print "anon_total_pages=" anon_total
      print "anon_selected_pages=" selected_pages
      print "anon_other_pages=" anon_total - selected_pages
    }
  ' "${source}" >"${destination}"
}

read_equals_field() {
  local source="$1"
  local key="$2"
  awk -F= -v wanted="${key}" '$1 == wanted { print $2; found = 1 }
       END { if (!found) exit 1 }' "${source}"
}

read_vma_anon_huge_kb() {
  local source="$1"
  local base="$2"
  local span="$3"
  local expected_name="$4"
  awk -v wanted_base="${base}" -v wanted_span="${span}" \
      -v expected_suffix="[anon:${expected_name}]" '
    function hex_digit(character) {
      character = tolower(character)
      if (character >= "0" && character <= "9")
        return character + 0
      return index("abcdef", character) + 9
    }
    function hex_to_decimal(value, number, position) {
      number = 0
      for (position = 1; position <= length(value); ++position)
        number = number * 16 + hex_digit(substr(value, position, 1))
      return number
    }
    BEGIN {
      wanted_base += 0
      wanted_end = wanted_base + wanted_span
    }
    /^[[:xdigit:]]+-[[:xdigit:]]+[[:space:]]/ {
      split($1, range, "-")
      range_start = hex_to_decimal(range[1])
      range_end = hex_to_decimal(range[2])
      selected = range_start == wanted_base && range_end == wanted_end
      if (selected) {
        ++selected_ranges
        if (length($0) >= length(expected_suffix) &&
            substr($0, length($0) - length(expected_suffix) + 1) == expected_suffix)
          ++named_ranges
      }
      next
    }
    selected && $1 == "AnonHugePages:" {
      print $2
      ++huge_fields
    }
    END {
      if (selected_ranges != 1 || named_ranges != 1 || huge_fields != 1)
        exit 1
    }
  ' "${source}"
}

HUGE_VMA_ERROR=""
validate_fully_huge_vma() {
  local source="$1"
  local base="$2"
  local span="$3"
  local label="$4"
  local expected_name="$5"
  local end=""
  local huge_kb=""
  local huge_bytes=""
  HUGE_VMA_ERROR=""

  if [[ "${base}" == 0 || "${span}" == 0 ]]; then
    HUGE_VMA_ERROR="${label} VMA range is invalid"
    return 1
  fi
  if uint_gt "${base}" 9223372036854775807 ||
     uint_gt "${span}" 9223372036854775807 ||
     [[ $((base % ARENA_ALIGNMENT_BYTES)) -ne 0 ]] ||
     [[ $((span % ARENA_ALIGNMENT_BYTES)) -ne 0 ]]; then
    HUGE_VMA_ERROR="${label} VMA is not exactly 2 MiB aligned and sized"
    return 1
  fi
  end="$(uint_add "${base}" "${span}")"
  if uint_gt "${end}" "${UINT64_MAX_VALUE}"; then
    HUGE_VMA_ERROR="${label} VMA range overflows uint64"
    return 1
  fi
  huge_kb="$(read_vma_anon_huge_kb "${source}" "${base}" "${span}" \
                                      "${expected_name}")" || {
    HUGE_VMA_ERROR="cannot identify exact ${label} VMA named [anon:${expected_name}] in smaps"
    return 1
  }
  if ! is_uint "${huge_kb}"; then
    HUGE_VMA_ERROR="${label} VMA AnonHugePages field is invalid"
    return 1
  fi
  huge_bytes="$(uint_multiply_small "${huge_kb}" 1024)"
  if [[ "$(normalize_uint "${huge_bytes}")" != \
        "$(normalize_uint "${span}")" ]]; then
    HUGE_VMA_ERROR="${label} VMA is not fully huge-page backed"
    return 1
  fi
  return 0
}

NUMA_VMA_ERROR=""
validate_vma_numa_residency() {
  local source="$1"
  local base="$2"
  local span="$3"
  local label="$4"
  NUMA_VMA_ERROR=""

  if ! awk -v wanted_base="${base}" -v wanted_span="${span}" \
      -v system_page_bytes="${PLAN_SYSTEM_PAGE_BYTES}" \
      -v selected_node="N${NUMA_NODE}" '
    function hex_digit(character) {
      character = tolower(character)
      if (character >= "0" && character <= "9")
        return character + 0
      return index("abcdef", character) + 9
    }
    function hex_to_decimal(value, number, position) {
      number = 0
      for (position = 1; position <= length(value); ++position)
        number = number * 16 + hex_digit(substr(value, position, 1))
      return number
    }
    BEGIN {
      wanted_base += 0
      expected_pages = wanted_span / system_page_bytes
    }
    /^[[:xdigit:]]+[[:space:]]/ {
      range_start = hex_to_decimal($1)
      if (range_start != wanted_base)
        next
      ++selected_lines
      anon_pages = -1
      anon_fields = 0
      node_pages = 0
      selected_pages = 0
      for (field_number = 2; field_number <= NF; ++field_number) {
        if ($field_number ~ /^anon=[0-9]+$/) {
          split($field_number, pair, "=")
          anon_pages = pair[2] + 0
          ++anon_fields
        } else if ($field_number ~ /^N[0-9]+=[0-9]+$/) {
          split($field_number, pair, "=")
          node_pages += pair[2]
          if (pair[1] == selected_node)
            selected_pages += pair[2]
        }
      }
      if (anon_fields == 1 && anon_pages == expected_pages &&
          node_pages == expected_pages && selected_pages == expected_pages)
        ++valid_lines
    }
    END {
      if (selected_lines != 1 || valid_lines != 1)
        exit 1
    }
  ' "${source}"; then
    NUMA_VMA_ERROR="${label} VMA is not wholly resident on NUMA node ${NUMA_NODE}"
    return 1
  fi
  return 0
}

READY_ARENA_BASES=()
READY_ARENA_MAPPED_BYTES=()
ARENA_FIELD_ERROR=""
read_ready_arena_fields() {
  local source="$1"
  local arena_index=""
  local arena_id=""
  local base=""
  local mapped_bytes=""
  local end=""
  READY_ARENA_BASES=()
  READY_ARENA_MAPPED_BYTES=()
  ARENA_FIELD_ERROR=""

  if ! read_record_field "${source}" itch_book_replay_ready \
       hot_arena_schema ||
     [[ "${RECORD_FIELD_VALUE}" != "${PLAN_HOT_ARENA_SCHEMA}" ]]; then
    ARENA_FIELD_ERROR="ready-marker hot-arena schema is missing or differs from storage plan"
    return 1
  fi

  for arena_index in "${!ARENA_IDS[@]}"; do
    arena_id="${ARENA_IDS[arena_index]}"
    if ! read_record_field "${source}" itch_book_replay_ready \
         "${arena_id}_base" || ! is_uint "${RECORD_FIELD_VALUE}"; then
      ARENA_FIELD_ERROR="ready marker lacks one numeric ${arena_id}_base"
      return 1
    fi
    base="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    if ! read_record_field "${source}" itch_book_replay_ready \
         "${arena_id}_mapped_bytes" ||
       ! is_uint "${RECORD_FIELD_VALUE}"; then
      ARENA_FIELD_ERROR="ready marker lacks one numeric ${arena_id}_mapped_bytes"
      return 1
    fi
    mapped_bytes="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    if [[ "${base}" == 0 ]] || uint_gt "${base}" 9223372036854775807 ||
       [[ $((base % ARENA_ALIGNMENT_BYTES)) -ne 0 ]]; then
      ARENA_FIELD_ERROR="ready-marker ${arena_id} base is not 2 MiB aligned"
      return 1
    fi
    if [[ "${mapped_bytes}" != \
          "${PLAN_ARENA_MAPPED_BYTES[arena_index]}" ]] ||
       [[ $((mapped_bytes % ARENA_ALIGNMENT_BYTES)) -ne 0 ]]; then
      ARENA_FIELD_ERROR="ready-marker ${arena_id} size differs from its aligned storage plan"
      return 1
    fi
    end="$(uint_add "${base}" "${mapped_bytes}")"
    if uint_gt "${end}" "${UINT64_MAX_VALUE}"; then
      ARENA_FIELD_ERROR="ready-marker ${arena_id} range overflows uint64"
      return 1
    fi
    READY_ARENA_BASES+=("${base}")
    READY_ARENA_MAPPED_BYTES+=("${mapped_bytes}")
  done
  return 0
}

cpp_quoted_value() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '"%s"' "${value}"
}

record_has_exact_quoted_field() {
  local source="$1"
  local record_name="$2"
  local key="$3"
  local expected="$4"
  local line=""
  local token=""
  line="$(awk -v record="${record_name}" '
    $1 == record { print; ++lines }
    END { if (lines != 1) exit 1 }
  ' "${source}")" || return 1
  token="${key}=$(cpp_quoted_value "${expected}")"
  [[ " ${line} " == *" ${token} "* ]]
}

BRANCH5_NATIVE_PREPARED_BOOKS=""
BRANCH5_NATIVE_RANGE_COUNT=""
BRANCH5_NATIVE_RANGE_BYTES=""
BRANCH5_NATIVE_RANGE_DIGEST=""
BRANCH5_NATIVE_ERROR=""
read_branch5_native_record() {
  local source="$1"
  local record_name="$2"
  local expected_manifest="$3"
  local key=""
  local value=""
  BRANCH5_NATIVE_ERROR=""

  if ! read_record_field "${source}" "${record_name}" hot_arena_schema ||
     [[ "${RECORD_FIELD_VALUE}" != branch5_native_v1 ]]; then
    BRANCH5_NATIVE_ERROR="${record_name} lacks branch5_native_v1 schema"
    return 1
  fi
  if ! read_record_field "${source}" "${record_name}" \
       native_range_manifest_schema ||
     [[ "${RECORD_FIELD_VALUE}" != branch5_native_ranges_v1 ]]; then
    BRANCH5_NATIVE_ERROR="${record_name} native manifest schema is invalid"
    return 1
  fi
  if ! record_has_exact_quoted_field "${source}" "${record_name}" \
       native_range_manifest "${expected_manifest}"; then
    BRANCH5_NATIVE_ERROR="${record_name} native manifest path is missing or unexpected"
    return 1
  fi

  for key in prefault native_prefault_complete book_universe_sealed \
             prepared_books native_range_count native_range_bytes \
             native_range_digest; do
    if ! read_record_field "${source}" "${record_name}" "${key}" ||
       ! is_uint "${RECORD_FIELD_VALUE}"; then
      BRANCH5_NATIVE_ERROR="${record_name} lacks one numeric ${key} field"
      return 1
    fi
    value="$(normalize_uint "${RECORD_FIELD_VALUE}")"
    case "${key}" in
      prefault|native_prefault_complete|book_universe_sealed)
        if [[ "${value}" != 1 ]]; then
          BRANCH5_NATIVE_ERROR="${record_name} reports incomplete native preparation"
          return 1
        fi
        ;;
      prepared_books) BRANCH5_NATIVE_PREPARED_BOOKS="${value}" ;;
      native_range_count) BRANCH5_NATIVE_RANGE_COUNT="${value}" ;;
      native_range_bytes) BRANCH5_NATIVE_RANGE_BYTES="${value}" ;;
      native_range_digest) BRANCH5_NATIVE_RANGE_DIGEST="${value}" ;;
    esac
  done
  if [[ "${BRANCH5_NATIVE_PREPARED_BOOKS}" == 0 ||
        "${BRANCH5_NATIVE_RANGE_BYTES}" == 0 ]] ||
     uint_gt "${BRANCH5_NATIVE_PREPARED_BOOKS}" 65535 ||
     uint_gt "${BRANCH5_NATIVE_RANGE_COUNT}" 262146 ||
     uint_gt "${BRANCH5_NATIVE_RANGE_BYTES}" "${UINT64_MAX_VALUE}" ||
     uint_gt "${BRANCH5_NATIVE_RANGE_DIGEST}" "${UINT64_MAX_VALUE}"; then
    BRANCH5_NATIVE_ERROR="${record_name} native range totals are invalid"
    return 1
  fi
  local expected_count=$((6 + 4 * BRANCH5_NATIVE_PREPARED_BOOKS))
  if [[ "${BRANCH5_NATIVE_RANGE_COUNT}" != "${expected_count}" ]]; then
    BRANCH5_NATIVE_ERROR="${record_name} native range count does not match prepared books"
    return 1
  fi
  if ! validate_branch5_capacity_extent \
       "${BRANCH5_NATIVE_PREPARED_BOOKS}" \
       "${BRANCH5_NATIVE_RANGE_COUNT}" \
       "${BRANCH5_NATIVE_RANGE_BYTES}" \
       "${PLANNED_BYTES}" "${BRANCH5_CAPACITY_POLICY}"; then
    BRANCH5_NATIVE_ERROR="${record_name} ${BRANCH5_CAPACITY_ERROR}"
    return 1
  fi
  return 0
}

validate_branch5_native_manifest() {
  local manifest="$1"
  local smaps="$2"
  local prepared_books="$3"
  local expected_count="$4"
  local expected_bytes="$5"
  local expected_digest="$6"
  shift 6
  local expected_global_sizes=("$@")
  local validation_output=""
  BRANCH5_NATIVE_ERROR=""

  if [[ ! -f "${manifest}" || -L "${manifest}" ]]; then
    BRANCH5_NATIVE_ERROR="branch5 native-range manifest is absent or not a regular file"
    return 1
  fi
  if [[ "${#expected_global_sizes[@]}" -ne 6 ]]; then
    BRANCH5_NATIVE_ERROR="branch5 native validator lacks six planned global sizes"
    return 1
  fi
  if ! validation_output="$(python3 - "${manifest}" "${smaps}" \
      "${prepared_books}" "${expected_count}" "${expected_bytes}" \
      "${expected_digest}" "${BRANCH5_CAPACITY_POLICY}" \
      "${expected_global_sizes[@]}" 2>&1 <<'PY'
import bisect
import collections
import re
import struct
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
smaps_argument = sys.argv[2]
expected_prepared, expected_count, expected_bytes, expected_digest = (
    int(value, 10) for value in sys.argv[3:7]
)
capacity_policy = sys.argv[7]
expected_global_sizes = tuple(int(value, 10) for value in sys.argv[8:14])
u64_max = (1 << 64) - 1
global_kinds = (
    "price_nodes", "price_leaves", "price_levels",
    "price_free_nodes", "price_free_leaves", "price_free_levels",
)
book_kinds = (
    "order_records", "order_free_indices", "order_occupancy",
    "order_ref_entries",
)
allowed_order_capacities = (65536, 262144, 1048576, 4194304)
active_locates = {85, 617, 1718, 5430, 5823, 6398}
hot_locates = {14, 331, 381, 3419, 3420, 5217}
ultra_hot_locates = {4118, 5628, 6449, 7291, 7804}

data = manifest_path.read_bytes()
if not data or not data.endswith(b"\n") or b"\r" in data:
    raise ValueError("manifest must be nonempty LF-terminated ASCII")
text = data.decode("ascii")
lines = text[:-1].split("\n")
header_pattern = re.compile(
    r"branch5_native_ranges schema=branch5_native_ranges_v1 "
    r"count=([0-9]+) bytes=([0-9]+) digest=([0-9]+)\Z"
)
row_pattern = re.compile(
    r"branch5_native_range ordinal=([0-9]+) kind=([a-z_]+) "
    r"locate=([0-9]+) base=([0-9]+) bytes=([0-9]+)\Z"
)
header = header_pattern.fullmatch(lines[0])
if header is None:
    raise ValueError("manifest header grammar/schema is invalid")
header_count, header_bytes, header_digest = (
    int(value, 10) for value in header.groups()
)
rows = []
for index, line in enumerate(lines[1:]):
    match = row_pattern.fullmatch(line)
    if match is None:
        raise ValueError(f"manifest row {index} grammar is invalid")
    ordinal, kind, locate, base, size = match.groups()
    ordinal, locate, base, size = map(int, (ordinal, locate, base, size))
    if ordinal != index:
        raise ValueError("manifest ordinals are not contiguous from zero")
    if not 0 <= locate <= 65535:
        raise ValueError("manifest locate exceeds the ITCH uint16 domain")
    if not 0 < base <= u64_max or not 0 < size <= u64_max:
        raise ValueError("manifest contains a zero/out-of-range span")
    if base + size > u64_max:
        raise ValueError("manifest span overflows uint64")
    rows.append((kind, locate, base, size))

if header_count != len(rows) or header_count != expected_count:
    raise ValueError("manifest count differs from header/ready marker")
if expected_count != 6 + 4 * expected_prepared:
    raise ValueError("manifest count differs from prepared-book count")
if tuple((kind, locate) for kind, locate, _, _ in rows[:6]) != tuple(
    (kind, 0) for kind in global_kinds
):
    raise ValueError("manifest global range order is invalid")
if tuple(row[3] for row in rows[:6]) != expected_global_sizes:
    raise ValueError("manifest global range sizes differ from storage plan")

prepared_locates = []
order_capacities = []
for start in range(6, len(rows), 4):
    group = rows[start:start + 4]
    locates = {row[1] for row in group}
    if len(group) != 4 or len(locates) != 1:
        raise ValueError("manifest prepared-book range group is incomplete")
    locate = next(iter(locates))
    if locate == 0 or tuple(row[0] for row in group) != book_kinds:
        raise ValueError("manifest prepared-book kind/locate order is invalid")
    order_sizes = tuple(row[3] for row in group)
    if order_sizes[0] % 32:
        raise ValueError("manifest order-record range is not capacity aligned")
    order_capacity = order_sizes[0] // 32
    expected_order_sizes = (
        32 * order_capacity,
        4 * order_capacity,
        order_capacity // 8,
        64 * order_capacity,
    )
    if (order_capacity not in allowed_order_capacities or
            order_sizes != expected_order_sizes):
        raise ValueError(
            "manifest per-book ranges differ from a canonical order tier"
        )
    prepared_locates.append(locate)
    order_capacities.append(order_capacity)
if len(prepared_locates) != expected_prepared or any(
    left >= right for left, right in zip(prepared_locates, prepared_locates[1:])
):
    raise ValueError("manifest prepared locates are not strictly ascending")
if capacity_policy == "pinned_trace_canonical_v1":
    if prepared_locates != list(range(1, 8714)):
        raise ValueError(
            "manifest prepared locates differ from the pinned full-trace universe"
        )
    for locate, order_capacity in zip(prepared_locates, order_capacities):
        expected_capacity = (
            4194304 if locate in ultra_hot_locates else
            1048576 if locate in hot_locates else
            262144 if locate in active_locates else
            65536
        )
        if order_capacity != expected_capacity:
            raise ValueError(
                "manifest order tier differs from the pinned trace directory"
            )
    if collections.Counter(order_capacities) != {
        65536: 8696,
        262144: 6,
        1048576: 6,
        4194304: 5,
    }:
        raise ValueError(
            "manifest order-tier counts differ from the pinned capacity evidence"
        )

total_bytes = sum(row[3] for row in rows)
if total_bytes > u64_max or total_bytes != header_bytes or total_bytes != expected_bytes:
    raise ValueError("manifest byte total differs from header/ready marker")
ordered_spans = sorted((base, base + size, kind, locate)
                       for kind, locate, base, size in rows)
if any(left[1] > right[0]
       for left, right in zip(ordered_spans, ordered_spans[1:])):
    raise ValueError("manifest native payload spans overlap")

fnv = 14695981039346656037
prime = 1099511628211
def update(payload):
    global fnv
    for byte in payload:
        fnv ^= byte
        fnv = (fnv * prime) & u64_max

update(b"branch5_native_ranges_v1")
for kind, locate, base, size in rows:
    encoded_kind = kind.encode("ascii")
    update(struct.pack("<Q", len(encoded_kind)))
    update(encoded_kind)
    update(struct.pack("<QQQ", locate, base, size))
if fnv != header_digest or fnv != expected_digest:
    raise ValueError("manifest digest differs from content/header/ready marker")

if smaps_argument != "-":
    header_re = re.compile(
        r"^([0-9a-fA-F]+)-([0-9a-fA-F]+)"
        r"\s+\S+\s+\S+\s+\S+\s+\S+(?:\s+(.*))?$"
    )
    anonymous = []
    for line in Path(smaps_argument).read_text(errors="strict").splitlines():
        match = header_re.match(line)
        if match is None:
            continue
        start, end = int(match.group(1), 16), int(match.group(2), 16)
        pathname = (match.group(3) or "").strip()
        if (not pathname or pathname == "[heap]" or
                pathname.startswith("[anon:")):
            anonymous.append((start, end))
    anonymous.sort()
    merged = []
    for start, end in anonymous:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    merged_starts = [start for start, _ in merged]
    for kind, locate, base, size in rows:
        end = base + size
        mapping_index = bisect.bisect_right(merged_starts, base) - 1
        if (mapping_index < 0 or
                end > merged[mapping_index][1]):
            raise ValueError(
                f"native span {kind}/{locate} is not contained in anonymous VMAs"
            )
PY
)"; then
    BRANCH5_NATIVE_ERROR="branch5 native manifest validation failed: ${validation_output}"
    return 1
  fi
  return 0
}

MEMORY_EVIDENCE_ERROR=""
validate_memory_snapshot() {
  local artifact_prefix="$1"
  local smaps_file="${artifact_prefix}.smaps_rollup.txt"
  local full_smaps_file="${artifact_prefix}.smaps.txt"
  local numa_maps_file="${artifact_prefix}.numa_maps.txt"
  local stdout_file="${artifact_prefix}.stdout.log"
  local summary_file="${artifact_prefix}.numa-summary.txt"
  local swap_kb=""
  local anonymous_kb=""
  local anonymous_bytes=""
  local anon_total=""
  local anon_selected=""
  local anon_numa_bytes=""
  local selected_scaled=""
  local required_scaled=""
  local arena_index=""
  local arena_id=""
  local arena_label=""
  local native_manifest="${artifact_prefix}.native-ranges.txt"
  local resident_minimum_bytes="${DERIVED_PLANNED_BYTES}"

  swap_kb="$(read_kb_field "${smaps_file}" Swap)" || {
    MEMORY_EVIDENCE_ERROR="smaps Swap field is unavailable"
    return 1
  }
  if [[ "$(normalize_uint "${swap_kb}")" != 0 ]]; then
    MEMORY_EVIDENCE_ERROR="smaps reports swapped benchmark pages"
    return 1
  fi
  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      if ! read_ready_arena_fields "${stdout_file}"; then
        MEMORY_EVIDENCE_ERROR="${ARENA_FIELD_ERROR}"
        return 1
      fi
      for arena_index in "${!ARENA_IDS[@]}"; do
        arena_id="${ARENA_IDS[arena_index]}"
        arena_label="${arena_id//_/-}"
        if ! validate_fully_huge_vma "${full_smaps_file}" \
             "${READY_ARENA_BASES[arena_index]}" \
             "${READY_ARENA_MAPPED_BYTES[arena_index]}" \
             "${arena_label}" "${ARENA_VMA_NAMES[arena_index]}"; then
          MEMORY_EVIDENCE_ERROR="${HUGE_VMA_ERROR}"
          return 1
        fi
        if ! validate_vma_numa_residency "${numa_maps_file}" \
             "${READY_ARENA_BASES[arena_index]}" \
             "${READY_ARENA_MAPPED_BYTES[arena_index]}" \
             "${arena_label}"; then
          MEMORY_EVIDENCE_ERROR="${NUMA_VMA_ERROR}"
          return 1
        fi
      done
      ;;
    branch5_native_ranges_v1)
      if ! read_branch5_native_record "${stdout_file}" \
           itch_book_replay_ready "${native_manifest}"; then
        MEMORY_EVIDENCE_ERROR="${BRANCH5_NATIVE_ERROR}"
        return 1
      fi
      if ! validate_branch5_native_manifest "${native_manifest}" \
           "${full_smaps_file}" "${BRANCH5_NATIVE_PREPARED_BOOKS}" \
           "${BRANCH5_NATIVE_RANGE_COUNT}" "${BRANCH5_NATIVE_RANGE_BYTES}" \
           "${BRANCH5_NATIVE_RANGE_DIGEST}" \
           "${PLAN_BRANCH5_PRICE_POOL_BYTES[@]}"; then
        MEMORY_EVIDENCE_ERROR="${BRANCH5_NATIVE_ERROR}"
        return 1
      fi
      resident_minimum_bytes="${BRANCH5_NATIVE_RANGE_BYTES}"
      ;;
    *)
      MEMORY_EVIDENCE_ERROR="unsupported hot-arena validation policy"
      return 1
      ;;
  esac
  anonymous_kb="$(read_kb_field "${smaps_file}" Anonymous)" || {
    MEMORY_EVIDENCE_ERROR="smaps Anonymous field is unavailable"
    return 1
  }
  if ! is_uint "${anonymous_kb}"; then
    MEMORY_EVIDENCE_ERROR="smaps Anonymous field is invalid"
    return 1
  fi
  anonymous_bytes="$(uint_multiply_small "${anonymous_kb}" 1024)"
  if ! uint_ge "${anonymous_bytes}" "${resident_minimum_bytes}"; then
    MEMORY_EVIDENCE_ERROR="anonymous resident bytes are below the schema-required live footprint"
    return 1
  fi
  anon_total="$(read_equals_field "${summary_file}" anon_total_pages)" || {
    MEMORY_EVIDENCE_ERROR="anonymous NUMA page total is unavailable"
    return 1
  }
  anon_selected="$(read_equals_field "${summary_file}" anon_selected_pages)" || {
    MEMORY_EVIDENCE_ERROR="selected-node anonymous page count is unavailable"
    return 1
  }
  if ! is_uint "${anon_total}" || ! is_uint "${anon_selected}" ||
     [[ "$(normalize_uint "${anon_total}")" == 0 ]]; then
    MEMORY_EVIDENCE_ERROR="anonymous NUMA page summary is invalid"
    return 1
  fi
  anon_numa_bytes="$(uint_multiply_small "${anon_total}" \
                                         "${PLAN_SYSTEM_PAGE_BYTES}")"
  if ! uint_ge "${anon_numa_bytes}" "${resident_minimum_bytes}"; then
    MEMORY_EVIDENCE_ERROR="anonymous NUMA pages are below the schema-required live footprint"
    return 1
  fi
  # numactl --membind should put effectively all anonymous capacity on the
  # selected node. Allow one percent for small pre-policy/runtime mappings.
  selected_scaled="$(uint_multiply_small "${anon_selected}" 100)"
  required_scaled="$(uint_multiply_small "${anon_total}" 99)"
  if uint_gt "${required_scaled}" "${selected_scaled}"; then
    MEMORY_EVIDENCE_ERROR="less than 99 percent of anonymous pages are on selected node ${NUMA_NODE}"
    return 1
  fi
  return 0
}

READY_IDENTITY_ERROR=""
validate_ready_replay_identity() {
  local source="$1"
  READY_IDENTITY_ERROR=""
  if ! read_record_field "${source}" itch_book_replay_ready \
       sample_schedule_id ||
     [[ "${RECORD_FIELD_VALUE}" != "${SAMPLE_SCHEDULE_ID}" ]]; then
    READY_IDENTITY_ERROR="sample schedule identity is missing or invalid"
    return 1
  fi
  if ! read_record_field "${source}" itch_book_replay_ready sample_every ||
     ! is_uint "${RECORD_FIELD_VALUE}" ||
     [[ "$(normalize_uint "${RECORD_FIELD_VALUE}")" != "${SAMPLE_EVERY}" ]]; then
    READY_IDENTITY_ERROR="sample interval differs from request"
    return 1
  fi
  if ! read_record_field "${source}" itch_book_replay_ready \
       warmup_book_messages || ! is_uint "${RECORD_FIELD_VALUE}" ||
     [[ "$(normalize_uint "${RECORD_FIELD_VALUE}")" != \
        "${WARMUP_BOOK_MESSAGES}" ]]; then
    READY_IDENTITY_ERROR="warmup differs from request"
    return 1
  fi
  return 0
}

monitor_process_memory() {
  local pid="$1"
  local artifact_prefix="$2"
  local stdout_file="$3"
  local gate_file="$4"
  local evidence_file="${artifact_prefix}.memory-evidence.txt"
  local captured=0
  : >"${evidence_file}"

  if [[ -z "${BASHPID:-}" ]]; then
    echo "monitor_affinity=failed reason=BASHPID_unavailable" >>"${evidence_file}"
    kill -TERM "${pid}" 2>/dev/null || true
    return 0
  fi
  if ! taskset -pc "${MONITOR_CPU}" "${BASHPID}" >>"${evidence_file}" 2>&1; then
    echo "monitor_affinity=failed" >>"${evidence_file}"
    kill -TERM "${pid}" 2>/dev/null || true
    return 0
  fi
  echo "monitor_affinity=cpu${MONITOR_CPU}" >>"${evidence_file}"

  while [[ -d "/proc/${pid}" ]]; do
    if [[ -r "/proc/${pid}/smaps_rollup" && -r "/proc/${pid}/smaps" &&
          -r "/proc/${pid}/numa_maps" ]]; then
      local rss_kb
      rss_kb="$(awk '$1 == "Rss:" { print $2; found = 1 }
                     END { if (!found) exit 1 }' "/proc/${pid}/smaps_rollup" 2>/dev/null || true)"
      if [[ -n "${rss_kb}" ]]; then
        local rss_bytes
        rss_bytes="$(uint_multiply_small "${rss_kb}" 1024)"
        printf 'timestamp_utc=%s pid=%s rss_bytes=%s\n' \
          "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${pid}" "${rss_bytes}" >>"${evidence_file}"
        # The benchmark emits and flushes this marker only after the manager's
        # mappings and timed-sample storage have completed their prefault pass.
        if [[ "${captured}" -eq 0 ]] &&
           grep -q '^itch_book_replay_ready ' "${stdout_file}" 2>/dev/null; then
          if cp "/proc/${pid}/smaps_rollup" "${artifact_prefix}.smaps_rollup.txt" 2>/dev/null &&
             cp "/proc/${pid}/smaps" "${artifact_prefix}.smaps.txt" 2>/dev/null &&
             cp "/proc/${pid}/numa_maps" "${artifact_prefix}.numa_maps.txt" 2>/dev/null; then
            if ! snapshot_numa_maps "${artifact_prefix}.numa_maps.txt" \
                 "${artifact_prefix}.numa-summary.txt"; then
              echo "post_prefault_snapshot=invalid reason=numa-summary-failed" >>"${evidence_file}"
              kill -TERM "${pid}" 2>/dev/null || true
              break
            fi
            if ! read_record_field "${stdout_file}" \
                 itch_book_replay_ready start_gate_enabled ||
               [[ "${RECORD_FIELD_VALUE}" != 1 ]]; then
              echo "post_prefault_snapshot=invalid start_gate=not-enabled" >>"${evidence_file}"
              kill -TERM "${pid}" 2>/dev/null || true
              break
            fi
            if ! validate_ready_replay_identity "${stdout_file}"; then
              echo "post_prefault_snapshot=invalid reason=${READY_IDENTITY_ERROR}" >>"${evidence_file}"
              kill -TERM "${pid}" 2>/dev/null || true
              break
            fi
            if ! validate_memory_snapshot "${artifact_prefix}"; then
              echo "post_prefault_snapshot=invalid reason=${MEMORY_EVIDENCE_ERROR}" >>"${evidence_file}"
              kill -TERM "${pid}" 2>/dev/null || true
              break
            fi
            if ! : >"${gate_file}"; then
              echo "post_prefault_snapshot=valid start_gate=release-failed" >>"${evidence_file}"
              kill -TERM "${pid}" 2>/dev/null || true
              break
            fi
            echo "post_prefault_snapshot=validated ready_marker=observed start_gate=released" >>"${evidence_file}"
            captured=1
            break
          fi
        fi
      fi
    fi
    sleep 1
  done

  if [[ "${captured}" -eq 0 ]]; then
    echo "post_prefault_snapshot=unavailable ready_marker=not-observed" >>"${evidence_file}"
  fi
  return 0
}

RUN_EXIT=0
MEMORY_EVIDENCE_CAPTURED=0
run_and_capture() {
  local label="$1"
  local stdout_file="$2"
  local stderr_file="$3"
  local artifact_prefix="$4"
  shift 4
  local command=("$@")
  local gate_file="${artifact_prefix}.start-gate"
  local native_manifest="${artifact_prefix}.native-ranges.txt"
  if [[ -e "${gate_file}" ]]; then
    die "start gate path already exists: ${gate_file}"
  fi
  if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
    if [[ -e "${native_manifest}" || -L "${native_manifest}" ]]; then
      die "native-range manifest path already exists: ${native_manifest}"
    fi
    command+=("--native-range-manifest=${native_manifest}")
  fi
  command+=("--start-gate-file=${gate_file}"
            "--start-gate-timeout-ms=600000")

  write_command_file "${artifact_prefix}.command.txt" "${command[@]}"
  echo "==> ${label}"
  print_command "${command[@]}"

  "${command[@]}" >"${stdout_file}" 2>"${stderr_file}" &
  local command_pid=$!
  ACTIVE_COMMAND_PID="${command_pid}"
  monitor_process_memory "${command_pid}" "${artifact_prefix}" \
    "${stdout_file}" "${gate_file}" &
  local monitor_pid=$!
  ACTIVE_MONITOR_PID="${monitor_pid}"

  if wait "${command_pid}"; then
    RUN_EXIT=0
  else
    RUN_EXIT=$?
  fi
  if wait "${monitor_pid}"; then
    :
  fi
  ACTIVE_COMMAND_PID=""
  ACTIVE_MONITOR_PID=""

  cat "${stdout_file}"
  if [[ -s "${stderr_file}" ]]; then
    cat "${stderr_file}" >&2
  fi
  if [[ -f "${artifact_prefix}.smaps_rollup.txt" &&
        -f "${artifact_prefix}.smaps.txt" &&
        -f "${artifact_prefix}.numa_maps.txt" ]]; then
    if validate_memory_snapshot "${artifact_prefix}"; then
      MEMORY_EVIDENCE_CAPTURED=1
      MEMORY_EVIDENCE_ERROR=""
    else
      MEMORY_EVIDENCE_CAPTURED=0
    fi
  else
    MEMORY_EVIDENCE_CAPTURED=0
    MEMORY_EVIDENCE_ERROR="post-prefault memory/NUMA snapshot unavailable"
  fi
  return 0
}

FIELD_VALUE=""
read_main_field() {
  local source="$1"
  local key="$2"
  if ! read_record_field "${source}" itch_book_replay "${key}"; then
    return 1
  fi
  FIELD_VALUE="${RECORD_FIELD_VALUE}"
  return 0
}

VALIDATION_ERROR=""
SAMPLE_COUNT_VALUE=""
validate_capacity_identity_record() {
  local source="$1"
  local record_name="$2"
  local key=""
  local expected=""
  for key in capacity_profile_bound capacity_evidence_schema \
             capacity_profile_name capacity_evidence_sha256 \
             capacity_corpus_manifest_sha256 capacity_profiler_sha256 \
             capacity_profiled_max_order_ref \
             capacity_profiled_unique_price_pages \
             capacity_minimum_direct_order_headroom \
             capacity_effective_direct_order_headroom \
             capacity_minimum_price_page_headroom \
             capacity_effective_price_page_headroom; do
    case "${key}" in
      capacity_profile_bound)
        expected="${PLAN_CAPACITY_PROFILE_BOUND}" ;;
      capacity_evidence_schema)
        expected="${PLAN_CAPACITY_EVIDENCE_SCHEMA}" ;;
      capacity_profile_name)
        expected="${PLAN_CAPACITY_PROFILE_NAME}" ;;
      capacity_evidence_sha256)
        expected="${PLAN_CAPACITY_EVIDENCE_SHA256}" ;;
      capacity_corpus_manifest_sha256)
        expected="${PLAN_CAPACITY_CORPUS_MANIFEST_SHA256}" ;;
      capacity_profiler_sha256)
        expected="${PLAN_CAPACITY_PROFILER_SHA256}" ;;
      capacity_profiled_max_order_ref)
        expected="${PLAN_CAPACITY_PROFILED_MAX_ORDER_REF}" ;;
      capacity_profiled_unique_price_pages)
        expected="${PLAN_CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}" ;;
      capacity_minimum_direct_order_headroom)
        expected="${PLAN_CAPACITY_MINIMUM_DIRECT_HEADROOM}" ;;
      capacity_effective_direct_order_headroom)
        expected="${PLAN_CAPACITY_EFFECTIVE_DIRECT_HEADROOM}" ;;
      capacity_minimum_price_page_headroom)
        expected="${PLAN_CAPACITY_MINIMUM_PRICE_HEADROOM}" ;;
      capacity_effective_price_page_headroom)
        expected="${PLAN_CAPACITY_EFFECTIVE_PRICE_HEADROOM}" ;;
    esac
    if ! read_record_field "${source}" "${record_name}" "${key}" ||
       [[ "${RECORD_FIELD_VALUE}" != "${expected}" ]]; then
      VALIDATION_ERROR="${record_name} ${key} is missing or differs from the retained capacity profile"
      return 1
    fi
  done
  return 0
}

validate_common_output() {
  local source="$1"
  local digest_enabled="$2"
  local start_gate_expected="$3"
  local value=""

  if ! read_main_field "${source}" hot_arena_schema ||
     [[ "${FIELD_VALUE}" != "${PLAN_HOT_ARENA_SCHEMA}" ]]; then
    VALIDATION_ERROR="final hot-arena schema is missing or differs from storage plan"
    return 1
  fi
  if [[ "${HOT_ARENA_POLICY}" == redesign_exact_v1 ]]; then
    validate_capacity_identity_record "${source}" itch_book_replay ||
      return 1
    validate_capacity_identity_record "${source}" itch_book_replay_ready ||
      return 1
  fi

  for key in records bytes prelude_records prelude_bytes \
             book_messages applied_book_mutations \
             sample_count sample_every warmup_book_messages \
             min_samples sample_capacity sample_storage_prefaulted \
             post_warmup_minor_faults post_warmup_major_faults prefault \
             storage_system_page_bytes rdtsc_overhead_ticks \
             rdtsc_ticks_per_second now_ns_overhead_ns \
             price_capacity_failures final_live_orders phase \
             mutation_digest_enabled semantic_mutation_digest_enabled \
             p50_ns p90_ns p99_ns p99_9_ns max_ns; do
    if ! read_main_field "${source}" "${key}"; then
      VALIDATION_ERROR="missing or ambiguous ${key} field"
      return 1
    fi
    value="${FIELD_VALUE}"
    if ! is_uint "${value}"; then
      VALIDATION_ERROR="non-numeric ${key} field"
      return 1
    fi
  done
  local main_records=""
  local main_bytes=""
  local main_prelude_records=""
  local main_prelude_bytes=""
  read_main_field "${source}" records
  main_records="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" bytes
  main_bytes="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" prelude_records
  main_prelude_records="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" prelude_bytes
  main_prelude_bytes="$(normalize_uint "${FIELD_VALUE}")"
  if [[ "${main_prelude_records}" == 0 || "${main_prelude_bytes}" == 0 ]] ||
     uint_gt "${main_prelude_records}" "${main_records}" ||
     uint_gt "${main_prelude_bytes}" "${main_bytes}"; then
    VALIDATION_ERROR="post-System-S prelude boundary is outside the replay extent"
    return 1
  fi
  local arena_index=""
  local arena_id=""
  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      for key in direct_order_slots fallback_buckets price_page_capacity \
                 effective_mapped_bytes \
                 effective_direct_orders_mapped_bytes \
                 effective_price_pages_mapped_bytes \
                 effective_descriptor_bytes effective_storage_bytes \
                 price_pages; do
        if ! read_main_field "${source}" "${key}" ||
           ! is_uint "${FIELD_VALUE}"; then
          VALIDATION_ERROR="missing or nonnumeric redesign ${key} field"
          return 1
        fi
      done
      for arena_index in "${!ARENA_IDS[@]}"; do
        arena_id="${ARENA_IDS[arena_index]}"
        if ! read_main_field "${source}" \
             "effective_${arena_id}_mapped_bytes" ||
           ! is_uint "${FIELD_VALUE}"; then
          VALIDATION_ERROR="missing or nonnumeric effective ${arena_id} size"
          return 1
        fi
      done
      ;;
    branch5_native_ranges_v1)
      for key in native_prefault_complete book_universe_sealed \
                 prepared_books native_range_count native_range_bytes \
                 native_range_digest price_internal_node_exhaustions \
                 price_leaf_exhaustions price_level_exhaustions; do
        if ! read_main_field "${source}" "${key}" ||
           ! is_uint "${FIELD_VALUE}"; then
          VALIDATION_ERROR="missing or nonnumeric branch5 ${key} field"
          return 1
        fi
      done
      ;;
  esac

  local ready_storage_bytes=""
  local ready_sample_capacity=""
  local ready_direct_orders_base=""
  local ready_direct_orders_mapped_bytes=""
  local ready_price_pages_base=""
  local ready_price_pages_mapped_bytes=""
  local ready_sample_every=""
  local ready_warmup_book_messages=""
  local ready_prelude_records=""
  local ready_prelude_bytes=""
  for key in prefault sample_capacity sample_storage_prefaulted \
             start_gate_enabled prelude_records prelude_bytes sample_every \
             warmup_book_messages; do
    if ! read_record_field "${source}" itch_book_replay_ready "${key}"; then
      VALIDATION_ERROR="missing or ambiguous ready-marker ${key} field"
      return 1
    fi
    if ! is_uint "${RECORD_FIELD_VALUE}"; then
      VALIDATION_ERROR="non-numeric ready-marker ${key} field"
      return 1
    fi
    case "${key}" in
      prefault|sample_storage_prefaulted)
        if [[ "$(normalize_uint "${RECORD_FIELD_VALUE}")" != 1 ]]; then
          VALIDATION_ERROR="ready marker precedes completed prefaulting"
          return 1
        fi
        ;;
      sample_capacity)
        ready_sample_capacity="$(normalize_uint "${RECORD_FIELD_VALUE}")"
        ;;
      start_gate_enabled)
        if [[ "$(normalize_uint "${RECORD_FIELD_VALUE}")" != \
              "${start_gate_expected}" ]]; then
          VALIDATION_ERROR="ready-marker start-gate mode differs from request"
          return 1
        fi
        ;;
      prelude_records)
        ready_prelude_records="$(normalize_uint "${RECORD_FIELD_VALUE}")" ;;
      prelude_bytes)
        ready_prelude_bytes="$(normalize_uint "${RECORD_FIELD_VALUE}")" ;;
      sample_every)
        ready_sample_every="$(normalize_uint "${RECORD_FIELD_VALUE}")"
        ;;
      warmup_book_messages)
        ready_warmup_book_messages="$(normalize_uint \
          "${RECORD_FIELD_VALUE}")"
        ;;
    esac
  done
  if [[ "${ready_prelude_records}" != "${main_prelude_records}" ||
        "${ready_prelude_bytes}" != "${main_prelude_bytes}" ]]; then
    VALIDATION_ERROR="ready/final post-System-S prelude boundary differs"
    return 1
  fi
  if [[ "${ready_sample_every}" != "${SAMPLE_EVERY}" ]]; then
    VALIDATION_ERROR="ready-marker sample interval differs from request"
    return 1
  fi
  if [[ "${ready_warmup_book_messages}" != \
        "${WARMUP_BOOK_MESSAGES}" ]]; then
    VALIDATION_ERROR="ready-marker warmup differs from request"
    return 1
  fi
  if ! read_record_field "${source}" itch_book_replay_ready \
       sample_schedule_id ||
     [[ "${RECORD_FIELD_VALUE}" != "${SAMPLE_SCHEDULE_ID}" ]]; then
    VALIDATION_ERROR="ready-marker sample schedule identity is missing or invalid"
    return 1
  fi
  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      for key in effective_storage_bytes direct_orders_base \
                 direct_orders_mapped_bytes price_pages_base \
                 price_pages_mapped_bytes; do
        if ! read_record_field "${source}" itch_book_replay_ready "${key}" ||
           ! is_uint "${RECORD_FIELD_VALUE}"; then
          VALIDATION_ERROR="missing or nonnumeric redesign ready-marker ${key}"
          return 1
        fi
        case "${key}" in
          effective_storage_bytes)
            ready_storage_bytes="$(normalize_uint "${RECORD_FIELD_VALUE}")" ;;
          direct_orders_base)
            ready_direct_orders_base="$(normalize_uint "${RECORD_FIELD_VALUE}")" ;;
          direct_orders_mapped_bytes)
            ready_direct_orders_mapped_bytes="$(normalize_uint \
              "${RECORD_FIELD_VALUE}")" ;;
          price_pages_base)
            ready_price_pages_base="$(normalize_uint "${RECORD_FIELD_VALUE}")" ;;
          price_pages_mapped_bytes)
            ready_price_pages_mapped_bytes="$(normalize_uint \
              "${RECORD_FIELD_VALUE}")" ;;
        esac
      done
      if ! read_ready_arena_fields "${source}"; then
        VALIDATION_ERROR="${ARENA_FIELD_ERROR}"
        return 1
      fi
      if [[ "${ready_storage_bytes}" != "${DERIVED_PLANNED_BYTES}" ]]; then
        VALIDATION_ERROR="ready-marker storage bytes differ from storage plan"
        return 1
      fi
      if [[ "${ready_direct_orders_base}" != \
            "${READY_ARENA_BASES[ARENA_DIRECT_INDEX]}" ||
            "${ready_direct_orders_mapped_bytes}" != \
            "${READY_ARENA_MAPPED_BYTES[ARENA_DIRECT_INDEX]}" ]]; then
        VALIDATION_ERROR="legacy direct-order ready fields differ from order_direct"
        return 1
      fi
      if [[ "${ready_price_pages_base}" != \
            "${READY_ARENA_BASES[ARENA_PRICE_PAGES_INDEX]}" ||
            "${ready_price_pages_mapped_bytes}" != \
            "${READY_ARENA_MAPPED_BYTES[ARENA_PRICE_PAGES_INDEX]}" ]]; then
        VALIDATION_ERROR="price-page ready fields differ from the arena plan"
        return 1
      fi
      ;;
    branch5_native_ranges_v1)
      local native_manifest="${source%.stdout.log}.native-ranges.txt"
      if ! read_branch5_native_record "${source}" itch_book_replay_ready \
           "${native_manifest}"; then
        VALIDATION_ERROR="${BRANCH5_NATIVE_ERROR}"
        return 1
      fi
      local ready_native_prepared="${BRANCH5_NATIVE_PREPARED_BOOKS}"
      local ready_native_count="${BRANCH5_NATIVE_RANGE_COUNT}"
      local ready_native_bytes="${BRANCH5_NATIVE_RANGE_BYTES}"
      local ready_native_digest="${BRANCH5_NATIVE_RANGE_DIGEST}"
      if ! read_branch5_native_record "${source}" itch_book_replay \
           "${native_manifest}"; then
        VALIDATION_ERROR="${BRANCH5_NATIVE_ERROR}"
        return 1
      fi
      if [[ "${BRANCH5_NATIVE_PREPARED_BOOKS}" != \
              "${ready_native_prepared}" ||
            "${BRANCH5_NATIVE_RANGE_COUNT}" != "${ready_native_count}" ||
            "${BRANCH5_NATIVE_RANGE_BYTES}" != "${ready_native_bytes}" ||
            "${BRANCH5_NATIVE_RANGE_DIGEST}" != "${ready_native_digest}" ]]; then
        VALIDATION_ERROR="branch5 ready/final native manifest identity differs"
        return 1
      fi
      if ! validate_branch5_native_manifest "${native_manifest}" - \
           "${ready_native_prepared}" "${ready_native_count}" \
           "${ready_native_bytes}" "${ready_native_digest}" \
           "${PLAN_BRANCH5_PRICE_POOL_BYTES[@]}"; then
        VALIDATION_ERROR="${BRANCH5_NATIVE_ERROR}"
        return 1
      fi
      ;;
  esac

  local aggregate_p50=""
  local aggregate_p90=""
  local aggregate_p99=""
  local aggregate_p999=""
  local aggregate_max=""
  read_main_field "${source}" p50_ns
  aggregate_p50="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" p90_ns
  aggregate_p90="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" p99_ns
  aggregate_p99="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" p99_9_ns
  aggregate_p999="$(normalize_uint "${FIELD_VALUE}")"
  read_main_field "${source}" max_ns
  aggregate_max="$(normalize_uint "${FIELD_VALUE}")"
  if uint_gt "${aggregate_p50}" "${aggregate_p90}" ||
     uint_gt "${aggregate_p90}" "${aggregate_p99}" ||
     uint_gt "${aggregate_p99}" "${aggregate_p999}" ||
     uint_gt "${aggregate_p999}" "${aggregate_max}"; then
    VALIDATION_ERROR="aggregate latency distribution is not monotonic"
    return 1
  fi

  read_main_field "${source}" records
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${EXPECTED_RECORDS}" ]] || {
    VALIDATION_ERROR="record count differs from gate"
    return 1
  }
  read_main_field "${source}" bytes
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${EXPECTED_BYTES}" ]] || {
    VALIDATION_ERROR="byte count differs from gate"
    return 1
  }
  read_main_field "${source}" book_messages
  local book_message_count
  book_message_count="$(normalize_uint "${FIELD_VALUE}")"
  [[ "${book_message_count}" != 0 ]] || {
    VALIDATION_ERROR="book-message count is zero"
    return 1
  }
  read_main_field "${source}" applied_book_mutations
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${book_message_count}" ]] || {
    VALIDATION_ERROR="not every book message was applied"
    return 1
  }
  read_main_field "${source}" sample_count
  SAMPLE_COUNT_VALUE="$(normalize_uint "${FIELD_VALUE}")"
  if ! uint_ge "${SAMPLE_COUNT_VALUE}" "${MIN_SAMPLES}"; then
    VALIDATION_ERROR="sample count is below minimum"
    return 1
  fi
  read_main_field "${source}" sample_capacity
  if ! uint_ge "${FIELD_VALUE}" "${SAMPLE_COUNT_VALUE}"; then
    VALIDATION_ERROR="sample capacity is below collected sample count"
    return 1
  fi
  if [[ "$(normalize_uint "${FIELD_VALUE}")" != \
        "${ready_sample_capacity}" ]]; then
    VALIDATION_ERROR="ready-marker sample capacity differs from final output"
    return 1
  fi
  read_main_field "${source}" sample_every
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${SAMPLE_EVERY}" ]] || {
    VALIDATION_ERROR="sample interval differs from request"
    return 1
  }
  read_main_field "${source}" warmup_book_messages
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${WARMUP_BOOK_MESSAGES}" ]] || {
    VALIDATION_ERROR="warmup differs from request"
    return 1
  }
  read_main_field "${source}" min_samples
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${MIN_SAMPLES}" ]] || {
    VALIDATION_ERROR="minimum-sample output differs from request"
    return 1
  }
  read_main_field "${source}" prefault
  [[ "${FIELD_VALUE}" == 1 ]] || {
    VALIDATION_ERROR="prefault was not enabled"
    return 1
  }
  read_main_field "${source}" sample_storage_prefaulted
  [[ "${FIELD_VALUE}" == 1 ]] || {
    VALIDATION_ERROR="timed sample storage was not prefaulted"
    return 1
  }
  read_main_field "${source}" post_warmup_minor_faults
  [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]] || {
    VALIDATION_ERROR="post-warmup minor page faults are nonzero"
    return 1
  }
  read_main_field "${source}" post_warmup_major_faults
  [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]] || {
    VALIDATION_ERROR="post-warmup major page faults are nonzero"
    return 1
  }
  read_main_field "${source}" mutation_digest_enabled
  [[ "${FIELD_VALUE}" == "${digest_enabled}" ]] || {
    VALIDATION_ERROR="mutation-digest mode differs from request"
    return 1
  }
  read_main_field "${source}" semantic_mutation_digest_enabled
  [[ "${FIELD_VALUE}" == "${digest_enabled}" ]] || {
    VALIDATION_ERROR="semantic-mutation-digest mode differs from request"
    return 1
  }
  if ! read_main_field "${source}" sample_strategy ||
     [[ "${FIELD_VALUE}" != fixed_seed_block_offset ]]; then
    VALIDATION_ERROR="fixed-seed block sampling was not reported"
    return 1
  fi
  if ! read_main_field "${source}" sample_schedule_id ||
     [[ "${FIELD_VALUE}" != "${SAMPLE_SCHEDULE_ID}" ]]; then
    VALIDATION_ERROR="sample schedule identity is missing or invalid"
    return 1
  fi
  if [[ "${digest_enabled}" == 1 ]]; then
    if ! read_main_field "${source}" mutation_digest ||
       ! is_uint "${FIELD_VALUE}"; then
      VALIDATION_ERROR="correctness output lacks a numeric mutation digest"
      return 1
    fi
    if ! read_main_field "${source}" semantic_mutation_digest ||
       ! is_uint "${FIELD_VALUE}"; then
      VALIDATION_ERROR="correctness output lacks a numeric semantic mutation digest"
      return 1
    fi
    if ! read_main_field "${source}" semantic_mutation_digest_schema ||
       [[ "${FIELD_VALUE}" != \
          applied_itch_book_semantics_v1_fnv1a64le ]]; then
      VALIDATION_ERROR="semantic mutation digest schema is missing or invalid"
      return 1
    fi
  else
    if read_main_field "${source}" mutation_digest ||
       read_main_field "${source}" semantic_mutation_digest ||
       read_main_field "${source}" semantic_mutation_digest_schema; then
      VALIDATION_ERROR="latency output unexpectedly contains correctness digests"
      return 1
    fi
  fi
  for key in sample_capacity rdtsc_ticks_per_second; do
    read_main_field "${source}" "${key}"
    if [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]]; then
      VALIDATION_ERROR="${key} must be nonzero for acceptance"
      return 1
    fi
  done
  read_main_field "${source}" storage_system_page_bytes
  [[ "$(normalize_uint "${FIELD_VALUE}")" == "${PLAN_SYSTEM_PAGE_BYTES}" ]] || {
    VALIDATION_ERROR="effective system page size differs from storage plan"
    return 1
  }
  read_main_field "${source}" price_capacity_failures
  [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]] || {
    VALIDATION_ERROR="price capacity failures are nonzero"
    return 1
  }
  case "${HOT_ARENA_POLICY}" in
    redesign_exact_v1)
      for key in direct_order_slots fallback_buckets price_page_capacity \
                 effective_storage_bytes; do
        read_main_field "${source}" "${key}"
        if [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]]; then
          VALIDATION_ERROR="${key} must be nonzero for redesign acceptance"
          return 1
        fi
      done
      read_main_field "${source}" direct_order_slots
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${PLAN_DIRECT_ORDER_SLOTS}" ]] || {
        VALIDATION_ERROR="effective direct-order capacity differs from storage plan"
        return 1
      }
      read_main_field "${source}" fallback_buckets
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${PLAN_FALLBACK_BUCKETS}" ]] || {
        VALIDATION_ERROR="effective fallback capacity differs from storage plan"
        return 1
      }
      read_main_field "${source}" price_page_capacity
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${PLAN_PRICE_PAGE_CAPACITY}" ]] || {
        VALIDATION_ERROR="effective price-page capacity differs from storage plan"
        return 1
      }
      read_main_field "${source}" effective_mapped_bytes
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${PLAN_MAPPED_ARRAY_BYTES}" ]] || {
        VALIDATION_ERROR="effective mapped bytes differ from storage plan"
        return 1
      }
      read_main_field "${source}" effective_direct_orders_mapped_bytes
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${PLAN_ARENA_MAPPED_BYTES[ARENA_DIRECT_INDEX]}" ]] || {
        VALIDATION_ERROR="legacy effective direct-order bytes differ from order_direct"
        return 1
      }
      for arena_index in "${!ARENA_IDS[@]}"; do
        arena_id="${ARENA_IDS[arena_index]}"
        read_main_field "${source}" "effective_${arena_id}_mapped_bytes"
        if [[ "$(normalize_uint "${FIELD_VALUE}")" != \
              "${PLAN_ARENA_MAPPED_BYTES[arena_index]}" ]]; then
          VALIDATION_ERROR="effective ${arena_id} bytes differ from storage plan"
          return 1
        fi
      done
      read_main_field "${source}" effective_descriptor_bytes
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${PLAN_DESCRIPTOR_BYTES}" ]] || {
        VALIDATION_ERROR="effective descriptor bytes differ from storage plan"
        return 1
      }
      read_main_field "${source}" effective_storage_bytes
      [[ "$(normalize_uint "${FIELD_VALUE}")" == \
          "${DERIVED_PLANNED_BYTES}" ]] || {
        VALIDATION_ERROR="effective storage bytes differ from preflight plan"
        return 1
      }
      read_main_field "${source}" price_pages
      if [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]] ||
         uint_gt "${FIELD_VALUE}" "${PLAN_PRICE_PAGE_CAPACITY}"; then
        VALIDATION_ERROR="committed price-page count is invalid"
        return 1
      fi
      ;;
    branch5_native_ranges_v1)
      for key in price_internal_node_exhaustions price_leaf_exhaustions \
                 price_level_exhaustions; do
        read_main_field "${source}" "${key}"
        if [[ "$(normalize_uint "${FIELD_VALUE}")" != 0 ]]; then
          VALIDATION_ERROR="branch5 ${key} is nonzero"
          return 1
        fi
      done
      ;;
  esac
  read_main_field "${source}" final_live_orders
  [[ "$(normalize_uint "${FIELD_VALUE}")" == 0 ]] || {
    VALIDATION_ERROR="final live-order count is nonzero"
    return 1
  }
  read_main_field "${source}" phase
  [[ "$(normalize_uint "${FIELD_VALUE}")" == 7 ]] || {
    VALIDATION_ERROR="final channel phase is not End of Messages"
    return 1
  }
  if [[ -n "${SAMPLE_CAPACITY}" ]]; then
    read_main_field "${source}" sample_capacity
    [[ "$(normalize_uint "${FIELD_VALUE}")" == "${SAMPLE_CAPACITY}" ]] || {
      VALIDATION_ERROR="effective sample capacity differs from request"
      return 1
    }
  fi

  local message_type=""
  local type_count=""
  local type_key=""
  local type_value=""
  local normalized_type_sample=""
  local type_record_count=""
  local type_sample_sum="0"
  local type_p50=""
  local type_p90=""
  local type_p99=""
  local type_p999=""
  local type_max=""
  local maximum_type_max="0"
  type_record_count="$(awk '$1 == "itch_book_replay_type" { ++lines }
                            END { print lines + 0 }' "${source}")"
  if [[ "${type_record_count}" != 7 ]]; then
    VALIDATION_ERROR="expected exactly seven type distributions"
    return 1
  fi
  for message_type in A F E C X D U; do
    type_p50=""
    type_p90=""
    type_p99=""
    type_p999=""
    type_max=""
    type_count="$(awk -v expected="type=${message_type}" '
      $1 == "itch_book_replay_type" {
        type_fields = 0
        matching_type = 0
        for (field_number = 2; field_number <= NF; ++field_number) {
          if (substr($field_number, 1, 5) == "type=")
            ++type_fields
          if ($field_number == expected)
            matching_type = 1
        }
        if (matching_type && type_fields == 1)
          ++matches
      }
      END { print matches + 0 }
    ' "${source}")"
    if [[ "${type_count}" != 1 ]]; then
      VALIDATION_ERROR="expected one type distribution for ${message_type}"
      return 1
    fi
    for type_key in sample_count p50_ns p90_ns p99_ns p99_9_ns max_ns; do
      type_value="$(awk -v expected="type=${message_type}" \
                            -v wanted="${type_key}=" '
        $1 == "itch_book_replay_type" {
          matching_type = 0
          type_fields = 0
          for (field_number = 2; field_number <= NF; ++field_number) {
            if (substr($field_number, 1, 5) == "type=")
              ++type_fields
            if ($field_number == expected)
              matching_type = 1
          }
          if (matching_type && type_fields == 1) {
            for (field_number = 2; field_number <= NF; ++field_number) {
              if (substr($field_number, 1, length(wanted)) == wanted) {
                print substr($field_number, length(wanted) + 1)
                ++matches
              }
            }
          }
        }
        END { if (matches != 1) exit 1 }
      ' "${source}")" || {
        VALIDATION_ERROR="type ${message_type} lacks one ${type_key} field"
        return 1
      }
      if ! is_uint "${type_value}"; then
        VALIDATION_ERROR="type ${message_type} has non-numeric ${type_key}"
        return 1
      fi
      if [[ "${type_key}" == sample_count &&
            "$(normalize_uint "${type_value}")" == 0 ]]; then
        VALIDATION_ERROR="type ${message_type} has no timed samples"
        return 1
      fi
      if [[ "${type_key}" == sample_count ]]; then
        normalized_type_sample="$(normalize_uint "${type_value}")"
        if uint_gt "${normalized_type_sample}" "${SAMPLE_COUNT_VALUE}"; then
          VALIDATION_ERROR="type ${message_type} sample count exceeds aggregate sample count"
          return 1
        fi
        type_sample_sum="$(uint_add "${type_sample_sum}" \
                                     "${normalized_type_sample}")"
      elif [[ "${type_key}" == p50_ns ]]; then
        type_p50="$(normalize_uint "${type_value}")"
      elif [[ "${type_key}" == p90_ns ]]; then
        type_p90="$(normalize_uint "${type_value}")"
      elif [[ "${type_key}" == p99_ns ]]; then
        type_p99="$(normalize_uint "${type_value}")"
      elif [[ "${type_key}" == p99_9_ns ]]; then
        type_p999="$(normalize_uint "${type_value}")"
      elif [[ "${type_key}" == max_ns ]]; then
        type_max="$(normalize_uint "${type_value}")"
      fi
    done
    if uint_gt "${type_p50}" "${type_p90}" ||
       uint_gt "${type_p90}" "${type_p99}" ||
       uint_gt "${type_p99}" "${type_p999}" ||
       uint_gt "${type_p999}" "${type_max}"; then
      VALIDATION_ERROR="type ${message_type} latency distribution is not monotonic"
      return 1
    fi
    if uint_gt "${type_max}" "${maximum_type_max}"; then
      maximum_type_max="${type_max}"
    fi
  done
  if [[ "$(normalize_uint "${type_sample_sum}")" != \
        "${SAMPLE_COUNT_VALUE}" ]]; then
    VALIDATION_ERROR="type sample counts do not sum to aggregate sample count"
    return 1
  fi
  if [[ "$(normalize_uint "${maximum_type_max}")" != \
        "${aggregate_max}" ]]; then
    VALIDATION_ERROR="maximum type latency differs from aggregate maximum"
    return 1
  fi
  return 0
}

append_reason() {
  local reason="$1"
  if [[ -z "${RUN_REASON}" ]]; then
    RUN_REASON="${reason}"
  else
    RUN_REASON="${RUN_REASON}; ${reason}"
  fi
}

PROVENANCE_VERIFICATION_ERROR=""
append_provenance_error() {
  local reason="$1"
  if [[ -z "${PROVENANCE_VERIFICATION_ERROR}" ]]; then
    PROVENANCE_VERIFICATION_ERROR="${reason}"
  else
    PROVENANCE_VERIFICATION_ERROR="${PROVENANCE_VERIFICATION_ERROR}; ${reason}"
  fi
}

verify_final_provenance() {
  local pass=1
  local binary_sha256_after=""
  local binary_stat_after=""
  local trace_sha256_after=""
  local trace_stat_after=""
  local harness_sha256_after=""
  local harness_stat_after=""
  local binary_archive_sha256_after=""
  local binary_archive_stat_after=""
  local harness_archive_sha256_after=""
  local harness_archive_stat_after=""
  local verifier_sha256_after=""
  local verifier_stat_after=""
  local verifier_archive_sha256_after=""
  local verifier_archive_stat_after=""
  local cmake_cache_sha256_after=""
  local cmake_cache_stat_after=""
  local cmake_cache_archive_sha256_after=""
  local cmake_cache_archive_stat_after=""
  local source_build_attestation_sha256_after=""
  local source_build_attestation_stat_after=""
  local source_tree_archive_sha256_after=""
  local source_tree_archive_stat_after=""
  local fresh_binary_archive_sha256_after=""
  local fresh_binary_archive_stat_after=""
  local fresh_cmake_cache_sha256_after=""
  local fresh_cmake_cache_stat_after=""
  local capacity_evidence_sha256_after="not_applicable"
  local capacity_evidence_stat_after="not_applicable"
  local capacity_evidence_archive_sha256_after="not_applicable"
  local capacity_evidence_archive_stat_after="not_applicable"
  local git_fingerprint_after=""
  local git_commit_after=""
  local git_parent_after=""
  local git_tree_after=""
  local git_branch_after=""
  local verification_file="${PROVENANCE_DIR}/provenance-verification.txt"

  PROVENANCE_VERIFICATION_ERROR=""
  echo "==> final trace, binary, harness, build, and Git provenance"

  if capture_file_state "${BINARY}" \
       "${PROVENANCE_DIR}/binary-source-after.state"; then
    binary_sha256_after="${FILE_STATE_SHA256}"
    binary_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${TRACE}" \
       "${PROVENANCE_DIR}/trace-after.state"; then
    trace_sha256_after="${FILE_STATE_SHA256}"
    trace_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${HARNESS_SOURCE}" \
       "${PROVENANCE_DIR}/harness-source-after.state"; then
    harness_sha256_after="${FILE_STATE_SHA256}"
    harness_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${BINARY_ARCHIVE}" \
       "${PROVENANCE_DIR}/binary-archive-after.state"; then
    binary_archive_sha256_after="${FILE_STATE_SHA256}"
    binary_archive_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${HARNESS_ARCHIVE}" \
       "${PROVENANCE_DIR}/harness-archive-after.state"; then
    harness_archive_sha256_after="${FILE_STATE_SHA256}"
    harness_archive_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if [[ -n "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" ]]; then
    if capture_file_state "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" \
         "${PROVENANCE_DIR}/capacity-evidence-source-after.state"; then
      capacity_evidence_sha256_after="${FILE_STATE_SHA256}"
      capacity_evidence_stat_after="${FILE_STATE_STAT}"
    else
      pass=0
      append_provenance_error "${FILE_STATE_ERROR}"
    fi
    if capture_file_state "${CAPACITY_EVIDENCE_ARCHIVE}" \
         "${PROVENANCE_DIR}/capacity-evidence-archive-after.state"; then
      capacity_evidence_archive_sha256_after="${FILE_STATE_SHA256}"
      capacity_evidence_archive_stat_after="${FILE_STATE_STAT}"
    else
      pass=0
      append_provenance_error "${FILE_STATE_ERROR}"
    fi
  fi
  if capture_file_state "${HOT_PATH_VERIFIER_SOURCE}" \
       "${PROVENANCE_DIR}/hot-path-verifier-source-after.state"; then
    verifier_sha256_after="${FILE_STATE_SHA256}"
    verifier_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${HOT_PATH_VERIFIER_ARCHIVE}" \
       "${PROVENANCE_DIR}/hot-path-verifier-archive-after.state"; then
    verifier_archive_sha256_after="${FILE_STATE_SHA256}"
    verifier_archive_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${CMAKE_CACHE}" \
       "${BUILD_PROVENANCE_DIR}/cmake-cache-source-after.state"; then
    cmake_cache_sha256_after="${FILE_STATE_SHA256}"
    cmake_cache_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${CMAKE_CACHE_ARCHIVE}" \
       "${BUILD_PROVENANCE_DIR}/cmake-cache-archive-after.state"; then
    cmake_cache_archive_sha256_after="${FILE_STATE_SHA256}"
    cmake_cache_archive_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${SOURCE_BUILD_ATTESTATION}" \
       "${SOURCE_BUILD_PROVENANCE_DIR}/source-build-attestation-after.state"; then
    source_build_attestation_sha256_after="${FILE_STATE_SHA256}"
    source_build_attestation_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${SOURCE_TREE_ARCHIVE}" \
       "${SOURCE_BUILD_PROVENANCE_DIR}/source-tree-archive-after.state"; then
    source_tree_archive_sha256_after="${FILE_STATE_SHA256}"
    source_tree_archive_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${SOURCE_BUILD_BINARY_ARCHIVE}" \
       "${SOURCE_BUILD_PROVENANCE_DIR}/fresh-binary-archive-after.state"; then
    fresh_binary_archive_sha256_after="${FILE_STATE_SHA256}"
    fresh_binary_archive_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_file_state "${SOURCE_BUILD_CMAKE_CACHE_ARCHIVE}" \
       "${SOURCE_BUILD_PROVENANCE_DIR}/cmake-cache-archive-after.state"; then
    fresh_cmake_cache_sha256_after="${FILE_STATE_SHA256}"
    fresh_cmake_cache_stat_after="${FILE_STATE_STAT}"
  else
    pass=0
    append_provenance_error "${FILE_STATE_ERROR}"
  fi
  if capture_git_state after; then
    git_fingerprint_after="${GIT_CAPTURE_FINGERPRINT}"
    git_commit_after="${GIT_CAPTURE_COMMIT}"
    git_parent_after="${GIT_CAPTURE_PARENT}"
    git_tree_after="${GIT_CAPTURE_TREE}"
    git_branch_after="${GIT_CAPTURE_BRANCH}"
  else
    pass=0
    append_provenance_error "cannot capture complete post-run Git state"
  fi

  if [[ "${binary_sha256_after}" != "${BINARY_SHA256_BEFORE}" ||
        "${binary_stat_after}" != "${BINARY_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "tested binary changed during acceptance"
  fi
  if [[ "${trace_sha256_after}" != "${TRACE_SHA256_BEFORE}" ||
        "${trace_stat_after}" != "${TRACE_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "trace changed during acceptance"
  fi
  if [[ "${harness_sha256_after}" != "${HARNESS_SHA256_BEFORE}" ||
        "${harness_stat_after}" != "${HARNESS_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "acceptance harness changed during acceptance"
  fi
  if [[ "${binary_archive_sha256_after}" != \
        "${BINARY_ARCHIVE_SHA256_BEFORE}" ||
        "${binary_archive_stat_after}" != \
        "${BINARY_ARCHIVE_STAT_BEFORE}" ||
        "${binary_archive_sha256_after}" != "${BINARY_SHA256_BEFORE}" ]]; then
    pass=0
    append_provenance_error "archived tested binary changed or no longer matches source"
  fi
  if [[ "${harness_archive_sha256_after}" != \
        "${HARNESS_ARCHIVE_SHA256_BEFORE}" ||
        "${harness_archive_stat_after}" != \
        "${HARNESS_ARCHIVE_STAT_BEFORE}" ||
        "${harness_archive_sha256_after}" != "${HARNESS_SHA256_BEFORE}" ]]; then
    pass=0
    append_provenance_error "archived harness changed or no longer matches source"
  fi
  if [[ -n "${CAPACITY_EVIDENCE_PROVENANCE_SOURCE}" ]] &&
     [[ "${capacity_evidence_sha256_after}" != \
          "${CAPACITY_EVIDENCE_SHA256_BEFORE}" ||
        "${capacity_evidence_stat_after}" != \
          "${CAPACITY_EVIDENCE_STAT_BEFORE}" ||
        "${capacity_evidence_archive_sha256_after}" != \
          "${CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE}" ||
        "${capacity_evidence_archive_stat_after}" != \
          "${CAPACITY_EVIDENCE_ARCHIVE_STAT_BEFORE}" ||
        "${capacity_evidence_archive_sha256_after}" != \
          "${capacity_evidence_sha256_after}" ||
        "${capacity_evidence_sha256_after}" != \
          "${CAPACITY_EVIDENCE_EXPECTED_SHA256}" ]]; then
    pass=0
    append_provenance_error "capacity evidence manifest changed or differs from its retained identity"
  fi
  if [[ "${verifier_sha256_after}" != \
        "${HOT_PATH_VERIFIER_SHA256_BEFORE}" ||
        "${verifier_stat_after}" != "${HOT_PATH_VERIFIER_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "hot-path verifier changed during acceptance"
  fi
  if [[ "${verifier_archive_sha256_after}" != \
        "${HOT_PATH_VERIFIER_ARCHIVE_SHA256_BEFORE}" ||
        "${verifier_archive_stat_after}" != \
        "${HOT_PATH_VERIFIER_ARCHIVE_STAT_BEFORE}" ||
        "${verifier_archive_sha256_after}" != \
        "${HOT_PATH_VERIFIER_SHA256_BEFORE}" ]]; then
    pass=0
    append_provenance_error "archived hot-path verifier changed or differs from source"
  fi
  if [[ "${cmake_cache_sha256_after}" != "${CMAKE_CACHE_SHA256_BEFORE}" ||
        "${cmake_cache_stat_after}" != "${CMAKE_CACHE_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "build CMakeCache changed during acceptance"
  fi
  if [[ "${cmake_cache_archive_sha256_after}" != \
        "${CMAKE_CACHE_ARCHIVE_SHA256_BEFORE}" ||
        "${cmake_cache_archive_stat_after}" != \
        "${CMAKE_CACHE_ARCHIVE_STAT_BEFORE}" ||
        "${cmake_cache_archive_sha256_after}" != \
        "${CMAKE_CACHE_SHA256_BEFORE}" ]]; then
    pass=0
    append_provenance_error "archived CMakeCache changed or differs from build cache"
  fi
  if [[ "${source_build_attestation_sha256_after}" != \
        "${SOURCE_BUILD_ATTESTATION_SHA256_BEFORE}" ||
        "${source_build_attestation_stat_after}" != \
        "${SOURCE_BUILD_ATTESTATION_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "clean-source build attestation changed during acceptance"
  fi
  if [[ "${source_tree_archive_sha256_after}" != \
        "${SOURCE_TREE_ARCHIVE_SHA256}" ||
        "${source_tree_archive_stat_after}" != \
        "${SOURCE_TREE_ARCHIVE_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "clean-source tree archive changed during acceptance"
  fi
  if [[ "${fresh_binary_archive_sha256_after}" != \
        "${SOURCE_BUILD_BINARY_ARCHIVE_SHA256_BEFORE}" ||
        "${fresh_binary_archive_stat_after}" != \
        "${SOURCE_BUILD_BINARY_ARCHIVE_STAT_BEFORE}" ||
        "${fresh_binary_archive_sha256_after}" != "${BINARY_SHA256_BEFORE}" ]]; then
    pass=0
    append_provenance_error "fresh clean-source binary archive changed or differs from tested binary"
  fi
  if [[ "${fresh_cmake_cache_sha256_after}" != \
        "${SOURCE_BUILD_CMAKE_CACHE_SHA256}" ||
        "${fresh_cmake_cache_stat_after}" != \
        "${SOURCE_BUILD_CMAKE_CACHE_ARCHIVE_STAT_BEFORE}" ]]; then
    pass=0
    append_provenance_error "fresh clean-source CMake cache changed during acceptance"
  fi
  if [[ "${git_fingerprint_after}" != "${GIT_FINGERPRINT_BEFORE}" ||
        "${git_commit_after}" != "${GIT_COMMIT_BEFORE}" ||
        "${git_parent_after}" != "${GIT_PARENT_BEFORE}" ||
        "${git_tree_after}" != "${GIT_TREE_BEFORE}" ||
        "${git_branch_after}" != "${GIT_BRANCH_BEFORE}" ]]; then
    pass=0
    append_provenance_error "Git branch, commit, or worktree content changed during acceptance"
  fi

  {
    echo "provenance_version=2"
    echo "binary_sha256_before=${BINARY_SHA256_BEFORE}"
    echo "binary_sha256_after=${binary_sha256_after:-unavailable}"
    echo "binary_archive_sha256_before=${BINARY_ARCHIVE_SHA256_BEFORE}"
    echo "binary_archive_sha256_after=${binary_archive_sha256_after:-unavailable}"
    echo "trace_sha256_before=${TRACE_SHA256_BEFORE}"
    echo "trace_sha256_after=${trace_sha256_after:-unavailable}"
    echo "capacity_evidence_sha256_before=${CAPACITY_EVIDENCE_SHA256_BEFORE}"
    echo "capacity_evidence_sha256_after=${capacity_evidence_sha256_after}"
    echo "capacity_evidence_archive_sha256_before=${CAPACITY_EVIDENCE_ARCHIVE_SHA256_BEFORE}"
    echo "capacity_evidence_archive_sha256_after=${capacity_evidence_archive_sha256_after}"
    echo "harness_sha256_before=${HARNESS_SHA256_BEFORE}"
    echo "harness_sha256_after=${harness_sha256_after:-unavailable}"
    echo "harness_archive_sha256_before=${HARNESS_ARCHIVE_SHA256_BEFORE}"
    echo "harness_archive_sha256_after=${harness_archive_sha256_after:-unavailable}"
    echo "hot_path_verifier_sha256_before=${HOT_PATH_VERIFIER_SHA256_BEFORE}"
    echo "hot_path_verifier_sha256_after=${verifier_sha256_after:-unavailable}"
    echo "cmake_cache_sha256_before=${CMAKE_CACHE_SHA256_BEFORE}"
    echo "cmake_cache_sha256_after=${cmake_cache_sha256_after:-unavailable}"
    echo "source_build_attestation_sha256_before=${SOURCE_BUILD_ATTESTATION_SHA256_BEFORE}"
    echo "source_build_attestation_sha256_after=${source_build_attestation_sha256_after:-unavailable}"
    echo "source_tree_archive_sha256_before=${SOURCE_TREE_ARCHIVE_SHA256}"
    echo "source_tree_archive_sha256_after=${source_tree_archive_sha256_after:-unavailable}"
    echo "fresh_binary_archive_sha256_before=${SOURCE_BUILD_BINARY_ARCHIVE_SHA256_BEFORE}"
    echo "fresh_binary_archive_sha256_after=${fresh_binary_archive_sha256_after:-unavailable}"
    echo "fresh_cmake_cache_sha256_before=${SOURCE_BUILD_CMAKE_CACHE_SHA256}"
    echo "fresh_cmake_cache_sha256_after=${fresh_cmake_cache_sha256_after:-unavailable}"
    echo "git_commit_before=${GIT_COMMIT_BEFORE}"
    echo "git_commit_after=${git_commit_after:-unavailable}"
    echo "git_parent_before=${GIT_PARENT_BEFORE}"
    echo "git_parent_after=${git_parent_after:-unavailable}"
    echo "git_tree_before=${GIT_TREE_BEFORE}"
    echo "git_tree_after=${git_tree_after:-unavailable}"
    echo "git_branch_before=${GIT_BRANCH_BEFORE}"
    echo "git_branch_after=${git_branch_after:-unavailable}"
    echo "git_fingerprint_before=${GIT_FINGERPRINT_BEFORE}"
    echo "git_fingerprint_after=${git_fingerprint_after:-unavailable}"
    if [[ "${pass}" -eq 1 ]]; then
      echo "result=PASS"
      echo "error=none"
    else
      echo "result=FAIL"
      echo "error=${PROVENANCE_VERIFICATION_ERROR}"
    fi
  } >"${verification_file}"
  [[ "${pass}" -eq 1 ]]
}

ALL_PASS=1
WORST_P50=0
WORST_P99=0
WORST_P999=0

for ((run = 1; run <= RUNS; ++run)); do
  printf -v run_id '%03d' "${run}"
  artifact_prefix="${OUTPUT_DIR}/latency-${run_id}"
  stdout_file="${artifact_prefix}.stdout.log"
  stderr_file="${artifact_prefix}.stderr.log"
  command=(
    "${BASE_COMMAND[@]}"
    "--max-p50-ns=${MAX_P50_NS}"
    "--max-p99-ns=${MAX_P99_NS}"
    "--max-p99-9-ns=${MAX_P999_NS}"
  )
  run_and_capture "latency run ${run}/${RUNS}" "${stdout_file}" \
    "${stderr_file}" "${artifact_prefix}" "${command[@]}"

  run_pass=1
  RUN_REASON=""
  p50=NA
  p99=NA
  p999=NA
  if [[ "${RUN_EXIT}" -ne 0 ]]; then
    run_pass=0
    append_reason "benchmark exit ${RUN_EXIT}"
  fi
  if ! validate_common_output "${stdout_file}" 0 1; then
    run_pass=0
    append_reason "${VALIDATION_ERROR}"
  fi
  if read_main_field "${stdout_file}" p50_ns; then p50="${FIELD_VALUE}"; fi
  if read_main_field "${stdout_file}" p99_ns; then p99="${FIELD_VALUE}"; fi
  if read_main_field "${stdout_file}" p99_9_ns; then p999="${FIELD_VALUE}"; fi
  if ! is_uint "${p50}" || ! is_uint "${p99}" || ! is_uint "${p999}"; then
    run_pass=0
    append_reason "invalid latency distribution"
  else
    p50="$(normalize_uint "${p50}")"
    p99="$(normalize_uint "${p99}")"
    p999="$(normalize_uint "${p999}")"
    if uint_gt "${p50}" "${MAX_P50_NS}"; then
      run_pass=0
      append_reason "p50 exceeds ceiling"
    fi
    if uint_gt "${p99}" "${MAX_P99_NS}"; then
      run_pass=0
      append_reason "p99 exceeds ceiling"
    fi
    if uint_gt "${p999}" "${MAX_P999_NS}"; then
      run_pass=0
      append_reason "p99.9 exceeds ceiling"
    fi
    if uint_gt "${p50}" "${WORST_P50}"; then WORST_P50="${p50}"; fi
    if uint_gt "${p99}" "${WORST_P99}"; then WORST_P99="${p99}"; fi
    if uint_gt "${p999}" "${WORST_P999}"; then WORST_P999="${p999}"; fi
  fi
  if [[ "${MEMORY_EVIDENCE_CAPTURED}" -ne 1 ]]; then
    run_pass=0
    append_reason "${MEMORY_EVIDENCE_ERROR}"
  fi

  if [[ "${run_pass}" -eq 1 ]]; then
    status=PASS
    detail="sample_count=${SAMPLE_COUNT_VALUE}"
  else
    status=FAIL
    detail="${RUN_REASON}"
    ALL_PASS=0
  fi
  printf 'latency\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${run_id}" "${status}" "${RUN_EXIT}" "${p50}" "${p99}" "${p999}" \
    "${detail}" "${stdout_file}" "${stderr_file}" >>"${SUMMARY_FILE}"
done

run_correctness_verification() {
  local label="$1"
  local expected_digest="$2"
  local expected_semantic_digest="$3"
  local artifact_name="$4"
  local artifact_prefix="${OUTPUT_DIR}/${artifact_name}"
  local stdout_file="${artifact_prefix}.stdout.log"
  local stderr_file="${artifact_prefix}.stderr.log"
  local command=("${BASE_COMMAND[@]}")
  local observed_digest=""
  local observed_semantic_digest=""
  if [[ -n "${expected_digest}" ]]; then
    command+=("--expect-mutation-digest=${expected_digest}")
  fi
  if [[ -n "${expected_semantic_digest}" ]]; then
    command+=("--expect-semantic-mutation-digest=${expected_semantic_digest}")
  fi
  run_and_capture "${label}" "${stdout_file}" "${stderr_file}" \
    "${artifact_prefix}" "${command[@]}"

  local pass=1
  RUN_REASON=""
  if [[ "${RUN_EXIT}" -ne 0 ]]; then
    pass=0
    append_reason "benchmark exit ${RUN_EXIT}"
  fi
  if ! validate_common_output "${stdout_file}" 1 1; then
    pass=0
    append_reason "${VALIDATION_ERROR}"
  elif ! read_main_field "${stdout_file}" mutation_digest; then
    pass=0
    append_reason "missing mutation digest"
  else
    observed_digest="$(normalize_uint "${FIELD_VALUE}")"
    if ! read_main_field "${stdout_file}" semantic_mutation_digest; then
      pass=0
      append_reason "missing semantic mutation digest"
    else
      observed_semantic_digest="$(normalize_uint "${FIELD_VALUE}")"
    fi
    if [[ -n "${expected_digest}" &&
          "${observed_digest}" != "${expected_digest}" ]]; then
      pass=0
      append_reason "mutation digest differs from expected"
    fi
    if [[ -n "${expected_semantic_digest}" &&
          "${observed_semantic_digest}" != "${expected_semantic_digest}" ]]; then
      pass=0
      append_reason "semantic mutation digest differs from expected"
    fi
  fi
  if [[ "${MEMORY_EVIDENCE_CAPTURED}" -ne 1 ]]; then
    pass=0
    append_reason "${MEMORY_EVIDENCE_ERROR}"
  fi
  if [[ "${pass}" -eq 1 ]]; then
    status=PASS
    detail="mutation_digest=${observed_digest} semantic_mutation_digest=${observed_semantic_digest}"
  else
    status=FAIL
    detail="${RUN_REASON}"
    ALL_PASS=0
  fi
  printf 'correctness\t%s\t%s\t%s\t-\t-\t-\t%s\t%s\t%s\n' \
    "${artifact_name}" "${status}" "${RUN_EXIT}" "${detail}" \
    "${stdout_file}" "${stderr_file}" >>"${SUMMARY_FILE}"
}

if [[ "${DISCOVER_DIGEST}" -eq 1 ]]; then
  artifact_prefix="${OUTPUT_DIR}/correctness-discovery"
  stdout_file="${artifact_prefix}.stdout.log"
  stderr_file="${artifact_prefix}.stderr.log"
  command=("${BASE_COMMAND[@]}" --mutation-digest)
  run_and_capture "correctness digest discovery" "${stdout_file}" \
    "${stderr_file}" "${artifact_prefix}" "${command[@]}"

  discovery_pass=1
  RUN_REASON=""
  discovered_digest=""
  discovered_semantic_digest=""
  if [[ "${RUN_EXIT}" -ne 0 ]]; then
    discovery_pass=0
    append_reason "benchmark exit ${RUN_EXIT}"
  fi
  if ! validate_common_output "${stdout_file}" 1 1; then
    discovery_pass=0
    append_reason "${VALIDATION_ERROR}"
  elif ! read_main_field "${stdout_file}" mutation_digest; then
    discovery_pass=0
    append_reason "missing mutation digest"
  elif ! is_uint "${FIELD_VALUE}"; then
    discovery_pass=0
    append_reason "invalid mutation digest"
  else
    discovered_digest="$(normalize_uint "${FIELD_VALUE}")"
    if ! read_main_field "${stdout_file}" semantic_mutation_digest; then
      discovery_pass=0
      append_reason "missing semantic mutation digest"
    elif ! is_uint "${FIELD_VALUE}"; then
      discovery_pass=0
      append_reason "invalid semantic mutation digest"
    else
      discovered_semantic_digest="$(normalize_uint "${FIELD_VALUE}")"
    fi
  fi
  if [[ "${MEMORY_EVIDENCE_CAPTURED}" -ne 1 ]]; then
    discovery_pass=0
    append_reason "${MEMORY_EVIDENCE_ERROR}"
  fi
  if [[ "${discovery_pass}" -eq 1 ]]; then
    printf 'correctness\tdiscovery\tPASS\t%s\t-\t-\t-\tmutation_digest=%s semantic_mutation_digest=%s\t%s\t%s\n' \
      "${RUN_EXIT}" "${discovered_digest}" "${discovered_semantic_digest}" \
      "${stdout_file}" "${stderr_file}" >>"${SUMMARY_FILE}"
    run_correctness_verification "correctness digest verification" \
      "${discovered_digest}" "${discovered_semantic_digest}" \
      correctness-verification
  else
    ALL_PASS=0
    printf 'correctness\tdiscovery\tFAIL\t%s\t-\t-\t-\t%s\t%s\t%s\n' \
      "${RUN_EXIT}" "${RUN_REASON}" "${stdout_file}" "${stderr_file}" >>"${SUMMARY_FILE}"
    printf 'correctness\tverification\tSKIPPED\t-\t-\t-\t-\tdiscovery failed\t-\t-\n' >>"${SUMMARY_FILE}"
  fi
elif [[ -n "${EXPECTED_MUTATION_DIGEST}" ||
        -n "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" ]]; then
  run_correctness_verification "correctness digest verification" \
    "${EXPECTED_MUTATION_DIGEST}" "${EXPECTED_SEMANTIC_MUTATION_DIGEST}" \
    correctness-verification
fi

if [[ "${RUN_PERF_STAT}" -eq 1 ]]; then
  perf_prefix="${OUTPUT_DIR}/hardware-counters"
  perf_stdout="${perf_prefix}.stdout.log"
  perf_stderr="${perf_prefix}.stderr.log"
  perf_counters="${perf_prefix}.perf-stat.csv"
  perf_command=(
    "${PERF_BIN}" stat
    -x ';'
    -o "${perf_counters}"
    -e "${PERF_EVENTS}"
    --
    "${BASE_COMMAND[@]}"
  )
  if [[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]; then
    perf_native_manifest="${perf_prefix}.native-ranges.txt"
    if [[ -e "${perf_native_manifest}" || -L "${perf_native_manifest}" ]]; then
      die "native-range manifest path already exists: ${perf_native_manifest}"
    fi
    perf_command+=("--native-range-manifest=${perf_native_manifest}")
  fi
  write_command_file "${perf_prefix}.command.txt" "${perf_command[@]}"
  echo "==> separate hardware-counter run"
  print_command "${perf_command[@]}"
  if "${perf_command[@]}" >"${perf_stdout}" 2>"${perf_stderr}"; then
    perf_exit=0
  else
    perf_exit=$?
  fi
  cat "${perf_stdout}"
  if [[ -s "${perf_stderr}" ]]; then
    cat "${perf_stderr}" >&2
  fi

  perf_pass=1
  RUN_REASON=""
  if [[ "${perf_exit}" -ne 0 ]]; then
    perf_pass=0
    append_reason "perf/benchmark exit ${perf_exit}"
  fi
  if ! validate_common_output "${perf_stdout}" 0 0; then
    perf_pass=0
    append_reason "${VALIDATION_ERROR}"
  fi
  if ! validate_perf_counters "${perf_counters}"; then
    perf_pass=0
    append_reason "${PERF_VALIDATION_ERROR}"
  fi

  if [[ "${perf_pass}" -eq 1 ]]; then
    printf 'hardware-counters\tperf-stat\tPASS\t%s\t-\t-\t-\tevents=%s counters=%s\t%s\t%s\n' \
      "${perf_exit}" "${PERF_EVENTS}" "${perf_counters}" "${perf_stdout}" \
      "${perf_stderr}" >>"${SUMMARY_FILE}"
  else
    ALL_PASS=0
    printf 'hardware-counters\tperf-stat\tFAIL\t%s\t-\t-\t-\t%s counters=%s\t%s\t%s\n' \
      "${perf_exit}" "${RUN_REASON}" "${perf_counters}" "${perf_stdout}" \
      "${perf_stderr}" >>"${SUMMARY_FILE}"
  fi
else
  printf 'hardware-counters\tperf-stat\tSKIPPED\t-\t-\t-\t-\t--no-perf-stat was explicit\t-\t-\n' >>"${SUMMARY_FILE}"
fi

if verify_final_provenance; then
  printf 'provenance\tpre-post\tPASS\t0\t-\t-\t-\ttrace/binary/harness/build/Git state stable\t%s\t-\n' \
    "${PROVENANCE_DIR}/provenance-verification.txt" >>"${SUMMARY_FILE}"
else
  ALL_PASS=0
  printf 'provenance\tpre-post\tFAIL\t1\t-\t-\t-\t%s\t%s\t-\n' \
    "${PROVENANCE_VERIFICATION_ERROR}" \
    "${PROVENANCE_DIR}/provenance-verification.txt" >>"${SUMMARY_FILE}"
fi

printf 'worst\tall-latency-runs\t-\t-\t%s\t%s\t%s\tceilings=%s/%s/%s\t-\t-\n' \
  "${WORST_P50}" "${WORST_P99}" "${WORST_P999}" \
  "${MAX_P50_NS}" "${MAX_P99_NS}" "${MAX_P999_NS}" >>"${SUMMARY_FILE}"

if [[ "${ALL_PASS}" -eq 1 ]]; then
  printf 'overall\t-\tPASS\t0\t-\t-\t-\tall requested runs passed\t-\t-\n' >>"${SUMMARY_FILE}"
  if ! finalize_artifact_manifest; then
    echo "acceptance=FAIL; artifact manifest could not be finalized" >&2
    exit 1
  fi
  echo "acceptance=PASS runs=${RUNS} worst_p50_ns=${WORST_P50} worst_p99_ns=${WORST_P99} worst_p99_9_ns=${WORST_P999}"
  echo "artifacts=${OUTPUT_DIR}"
  exit 0
fi

printf 'overall\t-\tFAIL\t1\t-\t-\t-\tone or more requested runs failed\t-\t-\n' >>"${SUMMARY_FILE}"
if ! finalize_artifact_manifest; then
  echo "acceptance harness: artifact manifest could not be finalized" >&2
fi
echo "acceptance=FAIL; every requested run is retained in ${OUTPUT_DIR}" >&2
exit 1
