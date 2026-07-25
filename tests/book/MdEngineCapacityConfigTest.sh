#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/md_engine" >&2
  exit 2
fi

binary=$1
script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
evidence_file="${script_dir}/../fixtures/book-capacity-evidence-v2.txt"
evidence_sha256=a64a97203610a321855988fcc4700e69223b4cd6b4e4bf819861d2f34719bd71
unset_args=(
  -u ASTRA_BOOK_CAPACITY_PROFILE
  -u ASTRA_BOOK_CAPACITY_EVIDENCE_FILE
  -u ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256
  -u ASTRA_ORDER_DIRECT_SLOTS
  -u ASTRA_ORDER_FALLBACK_BUCKETS
  -u ASTRA_PRICE_PAGE_CAPACITY
  -u ASTRA_PROFILED_MAX_ORDER_REF
  -u ASTRA_PROFILED_UNIQUE_PRICE_PAGES
  -u ASTRA_MIN_DIRECT_ORDER_HEADROOM
  -u ASTRA_MIN_PRICE_PAGE_HEADROOM
  -u ASTRA_BOOK_PREFAULT
)

expect_cli_failure() {
  local expected=$1
  shift
  local output
  if output=$(env "${unset_args[@]}" "$@" 2>&1); then
    echo "expected md_engine CLI failure containing: ${expected}" >&2
    echo "${output}" >&2
    exit 1
  fi
  if [[ ${output} != *"${expected}"* ]]; then
    echo "md_engine CLI failure did not contain: ${expected}" >&2
    echo "${output}" >&2
    exit 1
  fi
}

expect_cli_failure "port_a must be an integer" \
  "${binary}" 127.0.0.1 invalid
expect_cli_failure "port_a must be an integer" \
  "${binary}" 127.0.0.1 65536
expect_cli_failure "port_b must be an integer" \
  "${binary}" 127.0.0.1 9000 127.0.0.1 0
expect_cli_failure "channel_id must be an integer" \
  "${binary}" 127.0.0.1 9000 256
expect_cli_failure "ASTRA_UDP_BATCH_SIZE is not a valid in-range integer" \
  ASTRA_UDP_BATCH_SIZE=65 "${binary}" 127.0.0.1 9000
expect_cli_failure "Unknown ASTRA_RX: mystery" \
  ASTRA_RX=mystery "${binary}" 127.0.0.1 9000
expect_cli_failure "Unknown ASTRA_UDP_RX: mystery" \
  ASTRA_UDP_RX=mystery "${binary}" 127.0.0.1 9000
expect_cli_failure "ASTRA_LATENCY_METRICS must be one of" \
  ASTRA_LATENCY_METRICS=mystery "${binary}" 127.0.0.1 9000
expect_cli_failure "ASTRA_BOOK_CAPACITY_PROFILE is required" \
  "${binary}" 192.0.2.1 9000

expect_failure() {
  local expected=$1
  shift
  local output
  if output=$(env "${unset_args[@]}" "$@" \
      "${binary}" --book-storage-plan-only 2>&1); then
    echo "expected capacity-plan failure containing: ${expected}" >&2
    echo "${output}" >&2
    exit 1
  fi
  if [[ ${output} != *"${expected}"* ]]; then
    echo "capacity-plan failure did not contain: ${expected}" >&2
    echo "${output}" >&2
    exit 1
  fi
}

expect_failure "ASTRA_BOOK_CAPACITY_PROFILE is required"

expect_failure "ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256 is required" \
  ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-itch-20190130-acceptance-v1 \
  ASTRA_BOOK_PREFAULT=on

custom_profile=(
  ASTRA_BOOK_CAPACITY_PROFILE=nasdaq-prod-multiday-2026q3-v1
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="${evidence_file}"
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256="${evidence_sha256}"
  ASTRA_ORDER_DIRECT_SLOTS=1024
  ASTRA_ORDER_FALLBACK_BUCKETS=8
  ASTRA_PRICE_PAGE_CAPACITY=20
  ASTRA_PROFILED_MAX_ORDER_REF=900
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES=10
  ASTRA_MIN_DIRECT_ORDER_HEADROOM=100
  ASTRA_MIN_PRICE_PAGE_HEADROOM=5
  ASTRA_BOOK_PREFAULT=off
)

custom_output=$(env "${unset_args[@]}" "${custom_profile[@]}" \
  "${binary}" --book-storage-plan-only)
[[ ${custom_output} == *"name=nasdaq-prod-multiday-2026q3-v1"* ]]
[[ ${custom_output} == *"evidence_schema=astra_book_capacity_evidence_v2"* ]]
[[ ${custom_output} == *"evidence_sha256=${evidence_sha256}"* ]]
[[ ${custom_output} == *"corpus_manifest_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"* ]]
[[ ${custom_output} == *"profiler_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"* ]]
[[ ${custom_output} == *"profile_output_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"* ]]
[[ ${custom_output} == *"minimum_direct_order_headroom=100"* ]]
[[ ${custom_output} == *"effective_direct_order_headroom=123"* ]]
[[ ${custom_output} == *"minimum_price_page_headroom=5"* ]]
[[ ${custom_output} == *"effective_price_page_headroom=10"* ]]

expect_failure "ASTRA_BOOK_CAPACITY_EVIDENCE_FILE is required" \
  ASTRA_BOOK_CAPACITY_PROFILE=custom-v1 \
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
  ASTRA_ORDER_DIRECT_SLOTS=1024 \
  ASTRA_ORDER_FALLBACK_BUCKETS=8 \
  ASTRA_PRICE_PAGE_CAPACITY=20 \
  ASTRA_PROFILED_MAX_ORDER_REF=900 \
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES=10 \
  ASTRA_MIN_DIRECT_ORDER_HEADROOM=100 \
  ASTRA_MIN_PRICE_PAGE_HEADROOM=5

expect_failure "startup requires astra_book_capacity_evidence_v2" \
  "${custom_profile[@]}" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="${script_dir}/../fixtures/book-capacity-evidence-v1.txt" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=1ff9b1ecf60795a4be02975adbc6ab084202f95354bdedadbe77a4f5b511fb31

expect_failure "differs from the capacity evidence manifest" \
  "${custom_profile[@]}" ASTRA_MIN_DIRECT_ORDER_HEADROOM=124
expect_failure "differs from the capacity evidence manifest" \
  "${custom_profile[@]}" ASTRA_MIN_PRICE_PAGE_HEADROOM=11
expect_failure "differs from the capacity evidence manifest" \
  "${custom_profile[@]}" ASTRA_PROFILED_MAX_ORDER_REF=1024
expect_failure "only alphanumerics" \
  "${custom_profile[@]}" 'ASTRA_BOOK_CAPACITY_PROFILE=bad profile'
expect_failure "exactly 64 lowercase hex digits" \
  "${custom_profile[@]}" ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=ABCDEF
expect_failure "manifest SHA-256 differs from configured digest" \
  "${custom_profile[@]}" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=0000000000000000000000000000000000000000000000000000000000000000
expect_failure "profile_name differs from configured profile" \
  "${custom_profile[@]}" ASTRA_BOOK_CAPACITY_PROFILE=another-profile-v1
expect_failure "differs from the capacity evidence manifest" \
  "${custom_profile[@]}" ASTRA_ORDER_DIRECT_SLOTS=2048
expect_failure "is not a canonical positive integer" \
  "${custom_profile[@]}" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="${script_dir}/../fixtures/book-capacity-evidence-leading-zero.txt" \
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256=6728c8d00762de17de4126657dc4d06a97a0111d9233119a4301a408f652de17
