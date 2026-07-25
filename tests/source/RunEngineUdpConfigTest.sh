#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/run_engine_udp.sh" >&2
  exit 2
fi

script=$1

help_output=$(bash "${script}" --help)
[[ ${help_output} == *"ASTRA_BOOK_CAPACITY_PROFILE=<approved-profile>"* ]]
grep -Fq 'export ASTRA_RX=udp' "${script}"

if output=$(env -u ASTRA_BOOK_CAPACITY_PROFILE bash "${script}" 2>&1); then
  echo "expected UDP wrapper to reject a missing capacity profile" >&2
  echo "${output}" >&2
  exit 1
fi
[[ ${output} == *"ASTRA_BOOK_CAPACITY_PROFILE is required."* ]]
[[ ${output} == *"Load every value from one checksum-backed capacity manifest."* ]]
if [[ ${output} == *"Configuring UDP md_engine"* ]]; then
  echo "capacity profile was checked after build work" >&2
  echo "${output}" >&2
  exit 1
fi

if output=$(env ASTRA_BOOK_CAPACITY_PROFILE=custom-v2 \
    bash "${script}" 2>&1); then
  echo "expected UDP wrapper to reject incomplete capacity evidence" >&2
  echo "${output}" >&2
  exit 1
fi
[[ ${output} == *"ASTRA_BOOK_CAPACITY_EVIDENCE_FILE is required."* ]]
if [[ ${output} == *"Configuring UDP md_engine"* ]]; then
  echo "capacity evidence was checked after build work" >&2
  echo "${output}" >&2
  exit 1
fi
