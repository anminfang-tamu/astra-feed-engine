#!/usr/bin/env bash

# Host values are intentionally explicit. Export them from a host-local file
# such as build/dpdk-host.env before running a numbered dpdk_*.sh script. See
# docs/dpdk-aws-ec2-setup.md for the tested r7i.16xlarge example.

DPDK_CONFIG_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ASTRA_REPO_ROOT="${ASTRA_REPO_ROOT:-$(CDPATH= cd -- "${DPDK_CONFIG_DIR}/../.." && pwd)}"

ASTRA_FEED_IFACE="${ASTRA_FEED_IFACE:-}"
ASTRA_FEED_IP="${ASTRA_FEED_IP:-}"
ASTRA_FEED_PCI="${ASTRA_FEED_PCI:-}"
ASTRA_FEED_NUMA="${ASTRA_FEED_NUMA:-0}"
ASTRA_FEED_DRIVER="${ASTRA_FEED_DRIVER:-ena}"

# This is an additional fail-closed guard. The bind script also protects the
# main-table default-route interfaces and the interface carrying SSH.
ASTRA_PROTECTED_IFACE="${ASTRA_PROTECTED_IFACE:-}"
ASTRA_PROTECTED_PCI="${ASTRA_PROTECTED_PCI:-}"

ASTRA_ENGINE_CPU="${ASTRA_ENGINE_CPU:-2}"
ASTRA_ENI_ROUTE_TABLE="${ASTRA_ENI_ROUTE_TABLE:-1001}"
# Include the legacy table used by earlier two-ENI hosts. The bind script
# touches a table only after proving that every route/rule in it belongs to the
# feed ENI.
ASTRA_ENI_ROUTE_TABLES="${ASTRA_ENI_ROUTE_TABLES:-${ASTRA_ENI_ROUTE_TABLE} 101}"

ASTRA_DPDK_HUGE_DIR="${ASTRA_DPDK_HUGE_DIR:-/mnt/huge}"
ASTRA_DPDK_HUGE_PAGES="${ASTRA_DPDK_HUGE_PAGES:-2048}"
ASTRA_DPDK_FILE_PREFIX="${ASTRA_DPDK_FILE_PREFIX:-astra}"
ASTRA_DPDK_STATE_FILE="${ASTRA_DPDK_STATE_FILE:-${ASTRA_REPO_ROOT}/build/dpdk-ec2-state.env}"
ASTRA_DPDK_ROUTES_FILE="${ASTRA_DPDK_ROUTES_FILE:-${ASTRA_DPDK_STATE_FILE}.routes}"
ASTRA_DPDK_RULES_FILE="${ASTRA_DPDK_RULES_FILE:-${ASTRA_DPDK_STATE_FILE}.rules}"
ASTRA_DPDK_ADMISSION_FILE="${ASTRA_REPO_ROOT}/build/dpdk-capacity-admission.env"

# md_engine requires an approved deployment capacity configuration before it
# initializes DPDK. This default is only for the checked-in S061226
# profile; override both values together for another approved deployment.
ASTRA_BOOK_CAPACITY_FILE="${ASTRA_BOOK_CAPACITY_FILE:-${ASTRA_REPO_ROOT}/docs/book-capacity-evidence-S061226-v50.txt}"
ASTRA_BOOK_CAPACITY_FILE_SHA256="${ASTRA_BOOK_CAPACITY_FILE_SHA256:-55f5ba91d10c74ff28da877c3665a97dca69fe1c5a6572f64332ea56c30a5516}"
ASTRA_BOOK_PREFAULT="${ASTRA_BOOK_PREFAULT:-on}"

astra_capacity_manifest_value() {
  local key="$1"
  awk -F= -v key="${key}" '
    $1 == key {
      if (found) exit 2
      value = substr($0, length($1) + 2)
      found = 1
    }
    END {
      if (!found || value == "") exit 1
      print value
    }
  ' "${ASTRA_BOOK_CAPACITY_FILE}"
}

astra_load_book_capacity() {
  local actual_sha256
  local command_name
  local schema

  for command_name in awk sha256sum; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
      echo "Missing required command: ${command_name}" >&2
      return 1
    fi
  done
  if [[ ! -f "${ASTRA_BOOK_CAPACITY_FILE}" ]]; then
    echo "Capacity configuration not found: ${ASTRA_BOOK_CAPACITY_FILE}" >&2
    return 1
  fi
  actual_sha256="$(
    sha256sum "${ASTRA_BOOK_CAPACITY_FILE}" |
      awk '{print $1}'
  )"
  if [[ "${actual_sha256}" != "${ASTRA_BOOK_CAPACITY_FILE_SHA256}" ]]; then
    echo "Capacity configuration checksum mismatch." >&2
    return 1
  fi
  schema="$(astra_capacity_manifest_value schema)"
  if [[ "${schema}" != astra_book_capacity_evidence_v2 ]]; then
    echo "Unsupported capacity configuration schema: ${schema}" >&2
    return 1
  fi

  ASTRA_BOOK_CAPACITY_PROFILE="$(
    astra_capacity_manifest_value profile_name
  )"
  ASTRA_BOOK_CAPACITY_EVIDENCE_FILE="${ASTRA_BOOK_CAPACITY_FILE}"
  ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256="${actual_sha256}"
  ASTRA_ORDER_DIRECT_SLOTS="$(
    astra_capacity_manifest_value order_direct_slots
  )"
  ASTRA_ORDER_FALLBACK_BUCKETS="$(
    astra_capacity_manifest_value order_fallback_buckets
  )"
  ASTRA_PRICE_PAGE_CAPACITY="$(
    astra_capacity_manifest_value price_page_capacity
  )"
  ASTRA_PROFILED_MAX_ORDER_REF="$(
    astra_capacity_manifest_value profiled_max_order_ref
  )"
  ASTRA_PROFILED_UNIQUE_PRICE_PAGES="$(
    astra_capacity_manifest_value profiled_unique_price_pages
  )"
  ASTRA_MIN_DIRECT_ORDER_HEADROOM="$(
    astra_capacity_manifest_value minimum_direct_order_headroom
  )"
  ASTRA_MIN_PRICE_PAGE_HEADROOM="$(
    astra_capacity_manifest_value minimum_price_page_headroom
  )"
  export ASTRA_BOOK_CAPACITY_PROFILE
  export ASTRA_BOOK_CAPACITY_EVIDENCE_FILE
  export ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256
  export ASTRA_ORDER_DIRECT_SLOTS
  export ASTRA_ORDER_FALLBACK_BUCKETS
  export ASTRA_PRICE_PAGE_CAPACITY
  export ASTRA_PROFILED_MAX_ORDER_REF
  export ASTRA_PROFILED_UNIQUE_PRICE_PAGES
  export ASTRA_MIN_DIRECT_ORDER_HEADROOM
  export ASTRA_MIN_PRICE_PAGE_HEADROOM
  export ASTRA_BOOK_PREFAULT
}

astra_is_uint63_nonnegative() {
  local value="$1"
  [[ "${value}" =~ ^(0|[1-9][0-9]*)$ ]] || return 1
  if [[ "${#value}" -lt 19 ]]; then
    return 0
  fi
  if [[ "${#value}" -gt 19 ]]; then
    return 1
  fi
  [[ "${value}" < 9223372036854775807 ||
     "${value}" == 9223372036854775807 ]]
}

astra_is_uint63() {
  [[ "$1" != 0 ]] && astra_is_uint63_nonnegative "$1"
}

astra_is_valid_hugepage_count() {
  astra_is_uint63 "$1" && (( $1 <= 4398046511103 ))
}

astra_check_capacity_memory() {
  local required_memory_bytes="$1"
  local hugepage_bytes="$2"
  local configured_hugepage_bytes
  local node_meminfo
  local node_free_kib
  local node_free_bytes
  local hugepage_count_file
  local current_hugepages
  local incremental_hugepage_bytes
  local required_node_bytes
  local cgroup_root=/sys/fs/cgroup
  local cgroup_relative
  local cgroup_dir
  local cgroup_parent
  local memory_max
  local memory_current
  local memory_available
  local hugetlb_prefix
  local hugetlb_max
  local hugetlb_current
  local hugetlb_available
  local candidate

  if ! astra_is_uint63 "${required_memory_bytes}" ||
      ! astra_is_uint63 "${hugepage_bytes}" ||
      ! astra_is_valid_hugepage_count "${ASTRA_DPDK_HUGE_PAGES}" ||
      [[ ! "${ASTRA_FEED_NUMA}" =~ ^[0-9]+$ ]]; then
    echo "Capacity memory admission received an invalid value." >&2
    return 1
  fi
  configured_hugepage_bytes=$((
    ASTRA_DPDK_HUGE_PAGES * 2 * 1024 * 1024
  ))
  if [[ "${hugepage_bytes}" != "${configured_hugepage_bytes}" ]]; then
    echo "Capacity memory admission received a mismatched hugepage size." >&2
    return 1
  fi

  node_meminfo="/sys/devices/system/node/node${ASTRA_FEED_NUMA}/meminfo"
  if [[ ! -r "${node_meminfo}" ]]; then
    echo "NUMA node ${ASTRA_FEED_NUMA} is unavailable: ${node_meminfo}" >&2
    return 1
  fi
  node_free_kib="$(
    awk -v node="${ASTRA_FEED_NUMA}" '
      $1 == "Node" && $2 == node && $3 == "MemFree:" {
        print $4
        found=1
      }
      END {if (!found) exit 1}
    ' "${node_meminfo}"
  )"
  hugepage_count_file="/sys/devices/system/node/node${ASTRA_FEED_NUMA}/hugepages/hugepages-2048kB/nr_hugepages"
  if [[ ! -r "${hugepage_count_file}" ]]; then
    hugepage_count_file=/proc/sys/vm/nr_hugepages
  fi
  if [[ ! -r "${hugepage_count_file}" ]]; then
    echo "The 2 MiB hugepage count is not readable." >&2
    return 1
  fi
  current_hugepages="$(<"${hugepage_count_file}")"
  if [[ ! "${current_hugepages}" =~ ^[0-9]+$ ||
        "${#current_hugepages}" -gt 10 ]]; then
    echo "The current 2 MiB hugepage count is invalid." >&2
    return 1
  fi
  incremental_hugepage_bytes=0
  if (( current_hugepages < ASTRA_DPDK_HUGE_PAGES )); then
    incremental_hugepage_bytes=$((
      (ASTRA_DPDK_HUGE_PAGES - current_hugepages) * 2 * 1024 * 1024
    ))
  fi
  if (( required_memory_bytes >
        9223372036854775807 - incremental_hugepage_bytes )); then
    echo "Capacity memory admission exceeds signed 64-bit arithmetic." >&2
    return 1
  fi
  required_node_bytes=$((
    required_memory_bytes + incremental_hugepage_bytes
  ))
  if ! astra_is_uint63_nonnegative "${node_free_kib}" ||
      (( node_free_kib > 9007199254740991 )); then
    echo "NUMA node ${ASTRA_FEED_NUMA} reported an invalid MemFree value." >&2
    return 1
  fi
  node_free_bytes=$((node_free_kib * 1024))
  if (( node_free_bytes < required_node_bytes )); then
    cat >&2 <<EOF
NUMA node ${ASTRA_FEED_NUMA} does not have enough free memory for this engine.
required_bytes=${required_node_bytes}
node_free_bytes=${node_free_bytes}
Use a larger receiver or select an approved smaller capacity configuration
before binding the feed ENI.
EOF
    return 1
  fi

  if [[ ! -e "${cgroup_root}/cgroup.controllers" ]]; then
    echo "Capacity admission requires a unified cgroup v2 hierarchy." >&2
    return 1
  fi
  if [[ ! -r "${cgroup_root}/cgroup.controllers" ]]; then
    echo "The cgroup v2 controller list is not readable." >&2
    return 1
  fi
  if ! cgroup_relative="$(
    awk -F: '
      $1 == "0" && $2 == "" {
        print $3
        found=1
        exit
      }
      END {if (!found) exit 1}
    ' /proc/self/cgroup
  )"; then
    echo "Could not resolve the process cgroup v2 path." >&2
    return 1
  fi
  if [[ "${cgroup_relative}" != /* ||
        "/${cgroup_relative}/" == *"/../"* ]]; then
    echo "The process cgroup v2 path is invalid." >&2
    return 1
  fi
  if [[ "${cgroup_relative}" == / ]]; then
    cgroup_dir="${cgroup_root}"
  else
    cgroup_dir="${cgroup_root}${cgroup_relative}"
  fi
  if [[ ! -d "${cgroup_dir}" ]]; then
    echo "The process cgroup directory does not exist: ${cgroup_dir}" >&2
    return 1
  fi

  while :; do
    if [[ -e "${cgroup_dir}/memory.max" ||
          -e "${cgroup_dir}/memory.current" ]]; then
      if [[ ! -r "${cgroup_dir}/memory.max" ||
            ! -r "${cgroup_dir}/memory.current" ]]; then
        echo "The cgroup memory limit is not readable: ${cgroup_dir}" >&2
        return 1
      fi
      memory_max="$(<"${cgroup_dir}/memory.max")"
      memory_current="$(<"${cgroup_dir}/memory.current")"
      if [[ "${memory_max}" != max ]]; then
        if ! astra_is_uint63_nonnegative "${memory_max}" ||
            ! astra_is_uint63_nonnegative "${memory_current}"; then
          echo "The cgroup memory limit is invalid: ${cgroup_dir}" >&2
          return 1
        fi
        if (( memory_current > memory_max )); then
          echo "The cgroup memory usage exceeds its limit: ${cgroup_dir}" >&2
          return 1
        fi
        memory_available=$((memory_max - memory_current))
        if (( memory_available < required_memory_bytes )); then
          cat >&2 <<EOF
The process cgroup does not have enough available memory for this engine.
cgroup=${cgroup_dir}
required_bytes=${required_memory_bytes}
cgroup_available_bytes=${memory_available}
Raise the cgroup limit before binding the feed ENI.
EOF
          return 1
        fi
      fi
    fi

    hugetlb_prefix=""
    for candidate in hugetlb.2MB hugetlb.2048KB; do
      if [[ -e "${cgroup_dir}/${candidate}.max" ]]; then
        hugetlb_prefix="${candidate}"
        break
      fi
    done
    if [[ -n "${hugetlb_prefix}" ]]; then
      if [[ ! -r "${cgroup_dir}/${hugetlb_prefix}.max" ||
            ! -r "${cgroup_dir}/${hugetlb_prefix}.current" ]]; then
        echo "The cgroup hugetlb limit is not readable: ${cgroup_dir}" >&2
        return 1
      fi
      hugetlb_max="$(<"${cgroup_dir}/${hugetlb_prefix}.max")"
      hugetlb_current="$(<"${cgroup_dir}/${hugetlb_prefix}.current")"
      if [[ "${hugetlb_max}" != max ]]; then
        if ! astra_is_uint63_nonnegative "${hugetlb_max}" ||
            ! astra_is_uint63_nonnegative "${hugetlb_current}"; then
          echo "The cgroup hugetlb limit is invalid: ${cgroup_dir}" >&2
          return 1
        fi
        if (( hugetlb_current > hugetlb_max )); then
          echo "The cgroup hugetlb usage exceeds its limit: ${cgroup_dir}" >&2
          return 1
        fi
        hugetlb_available=$((hugetlb_max - hugetlb_current))
        if (( hugetlb_available < hugepage_bytes )); then
          cat >&2 <<EOF
The process cgroup cannot use the required 2 MiB hugepages.
cgroup=${cgroup_dir}
required_hugetlb_bytes=${hugepage_bytes}
cgroup_hugetlb_available_bytes=${hugetlb_available}
Raise the hugetlb cgroup limit before binding the feed ENI.
EOF
          return 1
        fi
      fi
    fi

    [[ "${cgroup_dir}" == "${cgroup_root}" ]] && break
    cgroup_parent="${cgroup_dir%/*}"
    if [[ "${cgroup_parent}" != "${cgroup_root}" &&
          "${cgroup_parent}" != "${cgroup_root}/"* ]]; then
      echo "The cgroup parent path escaped ${cgroup_root}." >&2
      return 1
    fi
    cgroup_dir="${cgroup_parent}"
  done
}

astra_require_capacity_admission() {
  local mode="${1:-check-memory}"
  local variable_name
  local actual_engine_sha256
  local expected_hugepage_bytes
  local expected_memory_bytes
  local expected_required_bytes

  if [[ "${mode}" != check-memory &&
        "${mode}" != check-runtime-memory &&
        "${mode}" != skip-memory ]]; then
    echo "Unknown capacity-admission mode: ${mode}" >&2
    return 2
  fi
  if ! astra_is_valid_hugepage_count "${ASTRA_DPDK_HUGE_PAGES}" ||
      [[
        ! "${ASTRA_FEED_NUMA}" =~ ^[0-9]+$ ]]; then
    echo "Current NUMA or hugepage configuration is invalid." >&2
    return 1
  fi
  if [[ ! -r "${ASTRA_DPDK_ADMISSION_FILE}" ]]; then
    echo "Missing DPDK capacity admission: ${ASTRA_DPDK_ADMISSION_FILE}" >&2
    echo "Run scripts/dpdk/dpdk_02_build.sh before binding or starting the engine." >&2
    return 1
  fi
  # shellcheck disable=SC1090
  source "${ASTRA_DPDK_ADMISSION_FILE}"
  for variable_name in \
    ASTRA_ADMITTED_CAPACITY_FILE \
    ASTRA_ADMITTED_CAPACITY_SHA256 \
    ASTRA_ADMITTED_ENGINE_SHA256 \
    ASTRA_ADMITTED_FEED_NUMA \
    ASTRA_ADMITTED_HUGE_PAGES \
    ASTRA_ADMITTED_PLANNED_BYTES \
    ASTRA_ADMITTED_REQUIRED_BYTES; do
    if [[ -z "${!variable_name-}" ]]; then
      echo "Capacity admission is missing ${variable_name}." >&2
      return 1
    fi
  done
  if ! astra_is_uint63 "${ASTRA_ADMITTED_PLANNED_BYTES}" ||
      ! astra_is_uint63 "${ASTRA_ADMITTED_REQUIRED_BYTES}" ||
      ! astra_is_valid_hugepage_count "${ASTRA_ADMITTED_HUGE_PAGES}" ||
      [[
        ! "${ASTRA_ADMITTED_FEED_NUMA}" =~ ^[0-9]+$ ||
        ! "${ASTRA_ADMITTED_CAPACITY_SHA256}" =~ ^[0-9a-f]{64}$ ||
        ! "${ASTRA_ADMITTED_ENGINE_SHA256}" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Capacity admission contains an invalid value." >&2
    return 1
  fi
  expected_hugepage_bytes=$((ASTRA_DPDK_HUGE_PAGES * 2 * 1024 * 1024))
  if (( ASTRA_ADMITTED_PLANNED_BYTES >
        9223372036854775807 -
        16 * 1024 * 1024 * 1024 -
        expected_hugepage_bytes )); then
    echo "Capacity admission exceeds signed 64-bit arithmetic." >&2
    return 1
  fi
  expected_memory_bytes=$((
    ASTRA_ADMITTED_PLANNED_BYTES +
    16 * 1024 * 1024 * 1024
  ))
  expected_required_bytes=$((
    expected_memory_bytes +
    expected_hugepage_bytes
  ))
  if [[ "${ASTRA_ADMITTED_CAPACITY_FILE}" != "${ASTRA_BOOK_CAPACITY_FILE}" ||
        "${ASTRA_ADMITTED_CAPACITY_SHA256}" != "${ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256}" ||
        "${ASTRA_ADMITTED_FEED_NUMA}" != "${ASTRA_FEED_NUMA}" ||
        "${ASTRA_ADMITTED_HUGE_PAGES}" != "${ASTRA_DPDK_HUGE_PAGES}" ||
        "${ASTRA_ADMITTED_REQUIRED_BYTES}" != "${expected_required_bytes}" ]]; then
    echo "Current DPDK configuration does not match the admitted capacity plan." >&2
    echo "Rerun scripts/dpdk/dpdk_02_build.sh with the values intended for steps 3 and 4." >&2
    return 1
  fi
  if [[ ! -x "${ASTRA_REPO_ROOT}/build/md_engine" ]]; then
    echo "The admitted md_engine binary is missing." >&2
    return 1
  fi
  actual_engine_sha256="$(
    sha256sum "${ASTRA_REPO_ROOT}/build/md_engine" |
      awk '{print $1}'
  )"
  if [[ "${actual_engine_sha256}" != "${ASTRA_ADMITTED_ENGINE_SHA256}" ]]; then
    echo "build/md_engine changed after capacity admission." >&2
    echo "Rerun scripts/dpdk/dpdk_02_build.sh before binding the feed ENI." >&2
    return 1
  fi
  if [[ "${mode}" == check-memory ]]; then
    astra_check_capacity_memory \
      "${expected_memory_bytes}" \
      "${expected_hugepage_bytes}"
  elif [[ "${mode}" == check-runtime-memory ]]; then
    astra_check_capacity_memory \
      "${expected_memory_bytes}" \
      "${expected_hugepage_bytes}"
  fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  cat <<EOF
repo=${ASTRA_REPO_ROOT}
feed_iface=${ASTRA_FEED_IFACE}
feed_ip=${ASTRA_FEED_IP}
feed_pci=${ASTRA_FEED_PCI}
feed_numa=${ASTRA_FEED_NUMA}
protected_iface=${ASTRA_PROTECTED_IFACE}
protected_pci=${ASTRA_PROTECTED_PCI}
engine_cpu=${ASTRA_ENGINE_CPU}
huge_dir=${ASTRA_DPDK_HUGE_DIR}
huge_pages=${ASTRA_DPDK_HUGE_PAGES}
admission_file=${ASTRA_DPDK_ADMISSION_FILE}
capacity_file=${ASTRA_BOOK_CAPACITY_FILE}
book_prefault=${ASTRA_BOOK_PREFAULT}
EOF
fi
