#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: OrderBookAcceptanceHarnessTest.sh HARNESS" >&2
  exit 2
fi

HARNESS="$1"
[[ -x "${HARNESS}" ]] || {
  echo "acceptance harness is not executable: ${HARNESS}" >&2
  exit 2
}

FIXTURE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/astra-acceptance-cli.XXXXXX")"
cleanup() {
  rm -rf -- "${FIXTURE_ROOT}"
}
trap cleanup EXIT HUP INT TERM

COMMON_ARGS=(
  --trace "${FIXTURE_ROOT}/missing trace.itch"
  --binary "${FIXTURE_ROOT}/missing replay binary"
  --cpu 2
  --numa-node 0
  --monitor-cpu 3
  --expect-records 11
  --expect-bytes 22
  --max-p50-ns 150
  --max-p99-ns 250
  --max-p99-9-ns 350
)
REDESIGN_ARGS=(
  "${COMMON_ARGS[@]}"
  --expect-hot-arena-schema redesign_v1
)

DISCOVERY_OUTPUT_DIR="${FIXTURE_ROOT}/must-not-exist-discovery"
DISCOVERY_OUTPUT="$("${HARNESS}" "${REDESIGN_ARGS[@]}" \
  --correctness-digest --hash-trace \
  --output-dir "${DISCOVERY_OUTPUT_DIR}" --dry-run)"
[[ ! -e "${DISCOVERY_OUTPUT_DIR}" ]]
[[ "$(grep -c '^latency_run=' <<<"${DISCOVERY_OUTPUT}")" -eq 5 ]]
grep -Fq -- '--mutation-digest' <<<"${DISCOVERY_OUTPUT}"
grep -Fq -- '--expect-mutation-digest=' <<<"${DISCOVERY_OUTPUT}"
grep -Fq -- '--expect-semantic-mutation-digest=' <<<"${DISCOVERY_OUTPUT}"

EXPECTED_OUTPUT_DIR="${FIXTURE_ROOT}/must-not-exist-expected"
EXPECTED_OUTPUT="$("${HARNESS}" "${REDESIGN_ARGS[@]}" \
  --expect-mutation-digest 42 \
  --expect-semantic-mutation-digest 43 \
  --output-dir "${EXPECTED_OUTPUT_DIR}" --dry-run)"
[[ ! -e "${EXPECTED_OUTPUT_DIR}" ]]
grep -Fq -- '--expect-mutation-digest=42' <<<"${EXPECTED_OUTPUT}"
grep -Fq -- '--expect-semantic-mutation-digest=43' <<<"${EXPECTED_OUTPUT}"

CAPACITY_SHA=1ff9b1ecf60795a4be02975adbc6ab084202f95354bdedadbe77a4f5b511fb31
CAPACITY_DRY_RUN="$("${HARNESS}" "${REDESIGN_ARGS[@]}" \
  --capacity-profile-name nasdaq-prod-multiday-2026q3-v1 \
  --capacity-evidence-file "${FIXTURE_ROOT}/capacity evidence.txt" \
  --capacity-evidence-sha256 "${CAPACITY_SHA}" \
  --direct-order-slots 1024 \
  --fallback-buckets 8 \
  --price-page-capacity 20 \
  --output-dir "${FIXTURE_ROOT}/must-not-exist-capacity" --dry-run)"
grep -Fq -- '--capacity-profile-name=nasdaq-prod-multiday-2026q3-v1' \
  <<<"${CAPACITY_DRY_RUN}"
grep -Fq -- '--capacity-evidence-file=' <<<"${CAPACITY_DRY_RUN}"
grep -Fq -- "--capacity-evidence-sha256=${CAPACITY_SHA}" \
  <<<"${CAPACITY_DRY_RUN}"

if "${HARNESS}" "${REDESIGN_ARGS[@]}" \
   --capacity-profile-name incomplete-v1 --dry-run \
   >"${FIXTURE_ROOT}/capacity-incomplete.stdout" \
   2>"${FIXTURE_ROOT}/capacity-incomplete.stderr"; then
  echo "harness accepted an incomplete capacity identity" >&2
  exit 1
fi
grep -Fq -- 'must be supplied together' \
  "${FIXTURE_ROOT}/capacity-incomplete.stderr"

if "${HARNESS}" "${REDESIGN_ARGS[@]}" \
   --capacity-profile-name invalid-sha-v1 \
   --capacity-evidence-file "${FIXTURE_ROOT}/unused" \
   --capacity-evidence-sha256 ABCDEF --dry-run \
   >"${FIXTURE_ROOT}/capacity-sha.stdout" \
   2>"${FIXTURE_ROOT}/capacity-sha.stderr"; then
  echo "harness accepted a malformed capacity evidence SHA" >&2
  exit 1
fi
grep -Fq -- 'exactly 64 lowercase hex digits' \
  "${FIXTURE_ROOT}/capacity-sha.stderr"

REJECTED_OUTPUT_DIR="${FIXTURE_ROOT}/must-not-exist-rejected"
if "${HARNESS}" "${REDESIGN_ARGS[@]}" --runs 4 \
   --output-dir "${REJECTED_OUTPUT_DIR}" --dry-run \
   >"${FIXTURE_ROOT}/runs-four.stdout" \
   2>"${FIXTURE_ROOT}/runs-four.stderr"; then
  echo "harness accepted fewer than five runs" >&2
  exit 1
fi
[[ ! -e "${REJECTED_OUTPUT_DIR}" ]]
grep -Fq -- '--runs must be at least 5' \
  "${FIXTURE_ROOT}/runs-four.stderr"

if "${HARNESS}" "${REDESIGN_ARGS[@]}" --correctness-digest \
   --expect-semantic-mutation-digest 43 --dry-run \
   >"${FIXTURE_ROOT}/conflict.stdout" \
   2>"${FIXTURE_ROOT}/conflict.stderr"; then
  echo "harness accepted conflicting digest modes" >&2
  exit 1
fi
grep -Fq -- 'expected digest options are mutually exclusive' \
  "${FIXTURE_ROOT}/conflict.stderr"

if "${HARNESS}" "${COMMON_ARGS[@]}" --dry-run \
   >"${FIXTURE_ROOT}/missing-schema.stdout" \
   2>"${FIXTURE_ROOT}/missing-schema.stderr"; then
  echo "dry-run accepted an unknown binary schema" >&2
  exit 1
fi
grep -Fq -- '--expect-hot-arena-schema is required with --dry-run' \
  "${FIXTURE_ROOT}/missing-schema.stderr"

HELP_OUTPUT="$("${HARNESS}" --help)"
grep -Fq -- 'Deprecated compatibility no-op' <<<"${HELP_OUTPUT}"
grep -Fq -- 'always hash the trace before and after replay' <<<"${HELP_OUTPUT}"
grep -Fq -- 'single-variant harness' <<<"${HELP_OUTPUT}"
grep -Fq -- 'explicit supported hot-arena schema' <<<"${HELP_OUTPUT}"
grep -Fq -- '--expect-hot-arena-schema NAME' <<<"${HELP_OUTPUT}"
grep -Fq -- 'fresh out-of-tree benchmark-only build graph' <<<"${HELP_OUTPUT}"

# `set -u` makes schema-dependent live policy invalid before the probe assigns
# this variable. Keep the assignment as its first executable occurrence.
FIRST_PROBED_SCHEMA_USE="$(grep -n 'PROBED_HOT_ARENA_SCHEMA' "${HARNESS}" |
  head -n 1)"
[[ "${FIRST_PROBED_SCHEMA_USE}" == *'PROBED_HOT_ARENA_SCHEMA="$(awk '* ]]

# Exercise the exact schema table and the same smaps/numa_maps validator
# functions used by the live harness. Function bodies end with an unindented
# brace, while braces inside their awk programs are indented.
extract_function() {
  local function_name="$1"
  sed -n "/^${function_name}() {$/,/^}$/p" "${HARNESS}"
}

VALIDATOR_LIBRARY="${FIXTURE_ROOT}/hot-arena-validator-functions.sh"
for function_name in is_uint configure_hot_arena_schema \
                     hot_arena_schema_matches_expectation normalize_uint \
                     uint_gt uint_add uint_multiply_small \
                     binary_matches_fresh_source_build \
                     read_vma_anon_huge_kb validate_fully_huge_vma \
                     validate_vma_numa_residency; do
  extract_function "${function_name}" >>"${VALIDATOR_LIBRARY}"
done
# shellcheck source=/dev/null
source "${VALIDATOR_LIBRARY}"

hot_arena_schema_matches_expectation redesign_v1 ""
hot_arena_schema_matches_expectation redesign_v1 redesign_v1
if hot_arena_schema_matches_expectation redesign_v1 unsupported_v1; then
  echo "hot-arena schema expectation accepted a mismatched live probe" >&2
  exit 1
fi

SUPPLIED_BINARY_FIXTURE="${FIXTURE_ROOT}/supplied-binary"
FRESH_BINARY_FIXTURE="${FIXTURE_ROOT}/fresh-binary"
STALE_BINARY_FIXTURE="${FIXTURE_ROOT}/stale-binary"
printf 'fresh binary bytes\n' >"${SUPPLIED_BINARY_FIXTURE}"
cp -- "${SUPPLIED_BINARY_FIXTURE}" "${FRESH_BINARY_FIXTURE}"
printf 'stale or manually substituted binary\n' >"${STALE_BINARY_FIXTURE}"
MATCHING_BINARY_SHA="$(sha256sum -- "${SUPPLIED_BINARY_FIXTURE}" | awk '{ print $1 }')"
STALE_BINARY_SHA="$(sha256sum -- "${STALE_BINARY_FIXTURE}" | awk '{ print $1 }')"
binary_matches_fresh_source_build \
  "${SUPPLIED_BINARY_FIXTURE}" "${MATCHING_BINARY_SHA}" \
  "${FRESH_BINARY_FIXTURE}" "${MATCHING_BINARY_SHA}"
if binary_matches_fresh_source_build \
   "${STALE_BINARY_FIXTURE}" "${STALE_BINARY_SHA}" \
   "${FRESH_BINARY_FIXTURE}" "${MATCHING_BINARY_SHA}"; then
  echo "harness accepted a stale or manually substituted binary" >&2
  exit 1
fi
if binary_matches_fresh_source_build \
   "${SUPPLIED_BINARY_FIXTURE}" "${STALE_BINARY_SHA}" \
   "${FRESH_BINARY_FIXTURE}" "${STALE_BINARY_SHA}"; then
  echo "harness trusted forged binary hash fields over file content" >&2
  exit 1
fi

if ! configure_hot_arena_schema redesign_v1; then
  echo "harness rejected its redesign_v1 arena schema" >&2
  exit 1
fi
[[ "${#ARENA_IDS[@]}" -eq 11 ]]
EXPECTED_ARENA_IDS=(
  order_direct order_fallback book_descriptors price_roots
  price_prepared_books price_pages price_page_owners price_page_summaries
  price_page_occupancy price_book_summaries price_book_occupancy
)
EXPECTED_VMA_NAMES=(
  astra-order-direct astra-order-fallback astra-book-descriptors
  astra-price-roots astra-price-prepared astra-price-pages
  astra-price-owners astra-price-summaries astra-price-page-bitmap
  astra-price-book-summaries astra-price-book-bitmap
)
[[ "${ARENA_IDS[*]}" == "${EXPECTED_ARENA_IDS[*]}" ]]
[[ "${ARENA_VMA_NAMES[*]}" == "${EXPECTED_VMA_NAMES[*]}" ]]
if configure_hot_arena_schema missing_schema; then
  echo "harness accepted an unknown hot-arena schema" >&2
  exit 1
fi
configure_hot_arena_schema redesign_v1

PLAN_SYSTEM_PAGE_BYTES=4096
NUMA_NODE=0
UINT64_MAX_VALUE=18446744073709551615
FIXTURE_BASE=4194304
FIXTURE_SPAN=2097152
VALID_SMAPS="${FIXTURE_ROOT}/valid.smaps"
cat >"${VALID_SMAPS}" <<'EOF'
00400000-00600000 rw-p 00000000 00:00 0 [anon:astra-test]
Size:               2048 kB
AnonHugePages:      2048 kB
EOF
VALID_NUMA_MAPS="${FIXTURE_ROOT}/valid.numa_maps"
cat >"${VALID_NUMA_MAPS}" <<'EOF'
400000 default anon=512 dirty=512 active=0 N0=512 kernelpagesize_kB=4
EOF

validate_fully_huge_vma "${VALID_SMAPS}" "${FIXTURE_BASE}" \
  "${FIXTURE_SPAN}" fixture astra-test
validate_vma_numa_residency "${VALID_NUMA_MAPS}" "${FIXTURE_BASE}" \
  "${FIXTURE_SPAN}" fixture

WRONG_NAME_SMAPS="${FIXTURE_ROOT}/wrong-name.smaps"
sed 's/astra-test/astra-wrong/' "${VALID_SMAPS}" >"${WRONG_NAME_SMAPS}"
if validate_fully_huge_vma "${WRONG_NAME_SMAPS}" "${FIXTURE_BASE}" \
   "${FIXTURE_SPAN}" fixture astra-test; then
  echo "harness accepted a VMA with the wrong Linux anonymous name" >&2
  exit 1
fi

WRONG_RANGE_SMAPS="${FIXTURE_ROOT}/wrong-range.smaps"
sed 's/00600000/00800000/' "${VALID_SMAPS}" >"${WRONG_RANGE_SMAPS}"
if validate_fully_huge_vma "${WRONG_RANGE_SMAPS}" "${FIXTURE_BASE}" \
   "${FIXTURE_SPAN}" fixture astra-test; then
  echo "harness accepted a VMA whose range exceeded the arena" >&2
  exit 1
fi

PARTIAL_HUGE_SMAPS="${FIXTURE_ROOT}/partial-huge.smaps"
sed 's/2048 kB/1024 kB/g' "${VALID_SMAPS}" >"${PARTIAL_HUGE_SMAPS}"
if validate_fully_huge_vma "${PARTIAL_HUGE_SMAPS}" "${FIXTURE_BASE}" \
   "${FIXTURE_SPAN}" fixture astra-test; then
  echo "harness accepted incomplete AnonHugePages residency" >&2
  exit 1
fi

OFF_NODE_NUMA="${FIXTURE_ROOT}/off-node.numa_maps"
cat >"${OFF_NODE_NUMA}" <<'EOF'
400000 default anon=512 dirty=512 active=0 N0=511 N1=1 kernelpagesize_kB=4
EOF
if validate_vma_numa_residency "${OFF_NODE_NUMA}" "${FIXTURE_BASE}" \
   "${FIXTURE_SPAN}" fixture; then
  echo "harness accepted an arena with one page on another NUMA node" >&2
  exit 1
fi

PARTIAL_NUMA="${FIXTURE_ROOT}/partial.numa_maps"
cat >"${PARTIAL_NUMA}" <<'EOF'
400000 default anon=511 dirty=511 active=0 N0=511 kernelpagesize_kB=4
EOF
if validate_vma_numa_residency "${PARTIAL_NUMA}" "${FIXTURE_BASE}" \
   "${FIXTURE_SPAN}" fixture; then
  echo "harness accepted an incompletely resident arena" >&2
  exit 1
fi
