#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/run_engine_dpdk.sh" >&2
  exit 2
fi

script=$1

help_output=$(bash "${script}" --help)
[[ ${help_output} == *"ASTRA_BOOK_CAPACITY_PROFILE=<approved-profile>"* ]]

if output=$(env -u ASTRA_BOOK_CAPACITY_PROFILE \
    ASTRA_DPDK_SKIP_BUILD=on bash "${script}" 2>&1); then
  echo "expected DPDK wrapper to reject a missing capacity profile" >&2
  echo "${output}" >&2
  exit 1
fi
[[ ${output} == *"ASTRA_BOOK_CAPACITY_PROFILE is required."* ]]
[[ ${output} == *"Load every value from one checksum-backed capacity manifest."* ]]
if [[ ${output} == *"Configuring md_engine"* ||
      ${output} == *"Using prebuilt DPDK md_engine"* ]]; then
  echo "capacity profile was checked after build/preflight work" >&2
  echo "${output}" >&2
  exit 1
fi

if output=$(env ASTRA_BOOK_CAPACITY_PROFILE=custom-v1 \
    ASTRA_DPDK_SKIP_BUILD=on bash "${script}" 2>&1); then
  echo "expected DPDK wrapper to reject incomplete capacity evidence" >&2
  echo "${output}" >&2
  exit 1
fi
[[ ${output} == *"ASTRA_BOOK_CAPACITY_EVIDENCE_FILE is required."* ]]
if [[ ${output} == *"Configuring md_engine"* ||
      ${output} == *"Using prebuilt DPDK md_engine"* ]]; then
  echo "capacity evidence was checked after build/preflight work" >&2
  echo "${output}" >&2
  exit 1
fi
