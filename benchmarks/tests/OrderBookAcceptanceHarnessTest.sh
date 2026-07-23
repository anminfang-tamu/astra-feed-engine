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
BRANCH5_ARGS=(
  "${COMMON_ARGS[@]}"
  --expect-hot-arena-schema branch5_native_v1
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

NATIVE_DRY_RUN="$("${HARNESS}" "${BRANCH5_ARGS[@]}" \
  --default-order-capacity 65536 \
  --price-node-capacity 163840 \
  --price-leaf-capacity 1048576 \
  --price-level-capacity 2097152 \
  --sample-capacity 8388608 \
  --planned-bytes 68719476736 \
  --reserve-bytes 17179869184 \
  --correctness-digest \
  --output-dir "${FIXTURE_ROOT}/must-not-exist-native" --dry-run)"
grep -Fq -- 'expected_hot_arena_schema=branch5_native_v1 schema_probe=skipped' \
  <<<"${NATIVE_DRY_RUN}"
grep -Fq -- '--default-order-capacity=65536' <<<"${NATIVE_DRY_RUN}"
grep -Fq -- '--price-node-capacity=163840' <<<"${NATIVE_DRY_RUN}"
grep -Fq -- '--price-leaf-capacity=1048576' <<<"${NATIVE_DRY_RUN}"
grep -Fq -- '--price-level-capacity=2097152' <<<"${NATIVE_DRY_RUN}"
grep -Fq -- 'hardware-counters.perf-stat.csv' <<<"${NATIVE_DRY_RUN}"
[[ "$(grep -c -- '--native-range-manifest=' <<<"${NATIVE_DRY_RUN}")" -eq 8 ]]

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

if "${HARNESS}" "${BRANCH5_ARGS[@]}" --direct-order-slots 1024 --dry-run \
   >"${FIXTURE_ROOT}/mixed-schema.stdout" \
   2>"${FIXTURE_ROOT}/mixed-schema.stderr"; then
  echo "branch5 dry-run accepted a redesign capacity option" >&2
  exit 1
fi
grep -Fq -- 'redesign capacity options are invalid for branch5_native_v1' \
  "${FIXTURE_ROOT}/mixed-schema.stderr"

if "${HARNESS}" "${BRANCH5_ARGS[@]}" --dry-run \
   >"${FIXTURE_ROOT}/branch5-unplanned.stdout" \
   2>"${FIXTURE_ROOT}/branch5-unplanned.stderr"; then
  echo "branch5 dry-run accepted an unresolved admission footprint" >&2
  exit 1
fi
grep -Fq -- 'branch5_native_v1 requires --planned-bytes' \
  "${FIXTURE_ROOT}/branch5-unplanned.stderr"

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
                     branch5_runtime_capacity_request_is_canonical \
                     branch5_plan_is_canonical \
                     validate_branch5_capacity_extent \
                     binary_matches_fresh_source_build \
                     read_vma_anon_huge_kb validate_fully_huge_vma \
                     validate_vma_numa_residency \
                     cpp_quoted_value record_has_exact_quoted_field \
                     validate_branch5_native_manifest; do
  extract_function "${function_name}" >>"${VALIDATOR_LIBRARY}"
done
# shellcheck source=/dev/null
source "${VALIDATOR_LIBRARY}"

hot_arena_schema_matches_expectation redesign_v1 ""
hot_arena_schema_matches_expectation redesign_v1 redesign_v1
if hot_arena_schema_matches_expectation redesign_v1 branch5_native_v1; then
  echo "hot-arena schema expectation accepted a mismatched live probe" >&2
  exit 1
fi

BRANCH5_PINNED_DEFAULT_ORDER_CAPACITY=65536
BRANCH5_PINNED_PRICE_NODE_CAPACITY=163840
BRANCH5_PINNED_PRICE_LEAF_CAPACITY=1048576
BRANCH5_PINNED_PRICE_LEVEL_CAPACITY=2097152
BRANCH5_PINNED_PRICE_POOL_BYTES=2456420352
BRANCH5_PINNED_PREPARED_BOOKS=8713
BRANCH5_PINNED_NATIVE_RANGE_COUNT=34858
BRANCH5_PINNED_NATIVE_RANGE_BYTES=62404952064
BRANCH5_PINNED_PLANNED_BYTES=68719476736
BRANCH5_CAPACITY_POLICY=default_binary_unbound_trace_v1

branch5_runtime_capacity_request_is_canonical "" "" "" ""
branch5_runtime_capacity_request_is_canonical \
  65536 163840 1048576 2097152
if branch5_runtime_capacity_request_is_canonical \
   65536 163840 "" 2097152; then
  echo "harness accepted partial branch5 capacity flags" >&2
  exit 1
fi
if branch5_runtime_capacity_request_is_canonical \
   65535 163840 1048576 2097152; then
  echo "harness accepted a noncanonical branch5 capacity flag" >&2
  exit 1
fi
branch5_plan_is_canonical 65536 163840 1048576 2097152 2456420352
if branch5_plan_is_canonical 65536 163840 1048576 2097151 2456420352; then
  echo "harness accepted a noncanonical branch5 storage plan" >&2
  exit 1
fi
validate_branch5_capacity_extent \
  8713 34858 62404952064 68719476736 pinned_trace_canonical_v1
if validate_branch5_capacity_extent \
   8713 34858 62404952064 62404952063 pinned_trace_canonical_v1; then
  echo "harness admitted fewer bytes than the branch5 native payload" >&2
  exit 1
fi
grep -Fq -- 'exceeds the admitted --planned-bytes' \
  <<<"${BRANCH5_CAPACITY_ERROR}"
if validate_branch5_capacity_extent \
   8712 34854 62404952064 68719476736 pinned_trace_canonical_v1; then
  echo "harness accepted the wrong pinned branch5 book universe" >&2
  exit 1
fi
grep -Fq -- 'differs from the pinned full-trace capacity profile' \
  <<<"${BRANCH5_CAPACITY_ERROR}"

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
configure_hot_arena_schema branch5_native_v1
[[ "${HOT_ARENA_POLICY}" == branch5_native_ranges_v1 ]]
[[ "${#ARENA_IDS[@]}" -eq 0 ]]
[[ "${#ARENA_VMA_NAMES[@]}" -eq 0 ]]
configure_hot_arena_schema redesign_v1

QUOTED_RECORD="${FIXTURE_ROOT}/quoted-ready.log"
printf 'itch_book_replay_ready native_range_manifest="%s"\n' \
  "${FIXTURE_ROOT}/manifest path.txt" >"${QUOTED_RECORD}"
record_has_exact_quoted_field "${QUOTED_RECORD}" itch_book_replay_ready \
  native_range_manifest "${FIXTURE_ROOT}/manifest path.txt"
if record_has_exact_quoted_field "${QUOTED_RECORD}" itch_book_replay_ready \
   native_range_manifest "${FIXTURE_ROOT}/wrong path.txt"; then
  echo "harness accepted an unexpected quoted native-manifest path" >&2
  exit 1
fi

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

NATIVE_MANIFEST="${FIXTURE_ROOT}/branch5-native-ranges.txt"
NATIVE_DIGEST="$(python3 - "${NATIVE_MANIFEST}" <<'PY'
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
kinds = (
    "price_nodes", "price_leaves", "price_levels", "price_free_nodes",
    "price_free_leaves", "price_free_levels", "order_records",
    "order_free_indices", "order_occupancy", "order_ref_entries",
)
sizes = (
    64, 80, 96, 112, 128, 144,
    32 * 65536, 4 * 65536, 65536 // 8, 64 * 65536,
)
rows = []
base = 0x410000
for ordinal, (kind, size) in enumerate(zip(kinds, sizes)):
    locate = 0 if ordinal < 6 else 7
    rows.append((ordinal, kind, locate, base, size))
    base += size + 0x1000
digest = 14695981039346656037
for byte in b"branch5_native_ranges_v1":
    digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
for _, kind, locate, base, size in rows:
    encoded = kind.encode("ascii")
    payload = struct.pack("<Q", len(encoded)) + encoded
    payload += struct.pack("<QQQ", locate, base, size)
    for byte in payload:
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
total = sum(row[4] for row in rows)
lines = [
    f"branch5_native_ranges schema=branch5_native_ranges_v1 "
    f"count={len(rows)} bytes={total} digest={digest}"
]
lines.extend(
    f"branch5_native_range ordinal={ordinal} kind={kind} locate={locate} "
    f"base={base} bytes={size}"
    for ordinal, kind, locate, base, size in rows
)
path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
print(digest)
PY
)"
NATIVE_TOTAL_BYTES=6562416
NATIVE_SMAPS="${FIXTURE_ROOT}/branch5.smaps"
cat >"${NATIVE_SMAPS}" <<'EOF'
00400000-00c00000 rw-p 00000000 00:00 0 [heap]
Size:               8192 kB
AnonHugePages:         0 kB
EOF
NATIVE_GLOBAL_SIZES=(64 80 96 112 128 144)
validate_branch5_native_manifest "${NATIVE_MANIFEST}" "${NATIVE_SMAPS}" \
  1 10 "${NATIVE_TOTAL_BYTES}" "${NATIVE_DIGEST}" \
  "${NATIVE_GLOBAL_SIZES[@]}"
validate_branch5_native_manifest "${NATIVE_MANIFEST}" - \
  1 10 "${NATIVE_TOTAL_BYTES}" "${NATIVE_DIGEST}" \
  "${NATIVE_GLOBAL_SIZES[@]}"

TAMPERED_MANIFEST="${FIXTURE_ROOT}/branch5-native-ranges-tampered.txt"
sed 's/kind=order_records/kind=order_occupancy/' "${NATIVE_MANIFEST}" \
  >"${TAMPERED_MANIFEST}"
if validate_branch5_native_manifest "${TAMPERED_MANIFEST}" \
   "${NATIVE_SMAPS}" 1 10 "${NATIVE_TOTAL_BYTES}" "${NATIVE_DIGEST}" \
   "${NATIVE_GLOBAL_SIZES[@]}"; then
  echo "harness accepted a tampered branch5 native-range manifest" >&2
  exit 1
fi

FILE_BACKED_SMAPS="${FIXTURE_ROOT}/branch5-file-backed.smaps"
cat >"${FILE_BACKED_SMAPS}" <<'EOF'
00400000-00800000 rw-p 00000000 08:01 1234 /tmp/not-anonymous
Size:               4096 kB
AnonHugePages:         0 kB
EOF
if validate_branch5_native_manifest "${NATIVE_MANIFEST}" \
   "${FILE_BACKED_SMAPS}" 1 10 "${NATIVE_TOTAL_BYTES}" "${NATIVE_DIGEST}" \
   "${NATIVE_GLOBAL_SIZES[@]}"; then
  echo "harness accepted branch5 native spans in a file-backed VMA" >&2
  exit 1
fi
