#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dpdk_ec2_config.sh
source "${SCRIPT_DIR}/dpdk_ec2_config.sh"

ALLOW_UNSAFE_NOIOMMU=0
USE_UNSAFE_NOIOMMU=0
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage:
  ./scripts/dpdk/dpdk_03_bind.sh [--allow-unsafe-noiommu] [--dry-run]

Reserve DPDK hugepages, remove Linux ownership from the configured secondary
feed ENI, and bind only that PCI device to vfio-pci.

Options:
  --allow-unsafe-noiommu  Explicitly allow VFIO without DMA isolation.
                         Use only on a dedicated, disposable host.
  --dry-run               Print privileged mutations without applying them.
  -h, --help              Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-unsafe-noiommu)
      ALLOW_UNSAFE_NOIOMMU=1
      ;;
    --dry-run)
      DRY_RUN=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

run_root() {
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    printf '+'
    if [[ "${EUID}" -ne 0 ]]; then
      printf ' sudo'
    fi
    printf ' %q' "$@"
    printf '\n'
    return
  fi

  if [[ "${EUID}" -eq 0 ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

driver_for_pci() {
  local pci="$1"
  local driver_path="/sys/bus/pci/devices/${pci}/driver"
  [[ -L "${driver_path}" ]] || return 1
  basename "$(readlink -f "${driver_path}")"
}

pci_for_iface() {
  local iface="$1"
  local device_path="/sys/class/net/${iface}/device"
  [[ -e "${device_path}" ]] || return 1
  basename "$(readlink -f "${device_path}")"
}

protect_iface() {
  local iface="$1"
  local reason="$2"
  local protected_pci

  [[ -n "${iface}" ]] || return
  if [[ "${iface}" == "${ASTRA_FEED_IFACE}" ]]; then
    echo "Refusing to bind ${ASTRA_FEED_IFACE}: it is the ${reason} interface." >&2
    exit 1
  fi

  protected_pci="$(pci_for_iface "${iface}" || true)"
  if [[ -n "${protected_pci}" &&
        "${protected_pci}" == "${ASTRA_FEED_PCI}" ]]; then
    echo "Refusing to bind ${ASTRA_FEED_PCI}: it belongs to the ${reason} interface ${iface}." >&2
    exit 1
  fi
}

if [[ "$(uname -s)" != Linux ]]; then
  echo "DPDK device binding is Linux-only." >&2
  exit 1
fi
for command_name in awk basename dirname findmnt ip mktemp modprobe mount \
                    mountpoint mv readlink rm sha256sum sort tee tr; do
  require_command "${command_name}"
done
if [[ "${EUID}" -ne 0 && "${DRY_RUN}" -eq 0 ]]; then
  require_command sudo
fi

if [[ ! "${ASTRA_FEED_IFACE}" =~ ^[[:alnum:]_.:-]+$ ]]; then
  echo "Invalid ASTRA_FEED_IFACE: ${ASTRA_FEED_IFACE}" >&2
  exit 2
fi
if [[ ! "${ASTRA_FEED_PCI}" =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[[:xdigit:]]$ ]]; then
  echo "Invalid ASTRA_FEED_PCI: ${ASTRA_FEED_PCI}" >&2
  exit 2
fi
if [[ ! "${ASTRA_FEED_NUMA}" =~ ^[0-9]+$ ]]; then
  echo "ASTRA_FEED_NUMA must be a non-negative NUMA node." >&2
  exit 2
fi
if [[ ! "${ASTRA_DPDK_HUGE_PAGES}" =~ ^[1-9][0-9]*$ ]]; then
  echo "ASTRA_DPDK_HUGE_PAGES must be a positive integer." >&2
  exit 2
fi
if [[ ! "${ASTRA_ENI_ROUTE_TABLE}" =~ ^[1-9][0-9]*$ ]]; then
  echo "ASTRA_ENI_ROUTE_TABLE must be a positive integer." >&2
  exit 2
fi
if [[ ! "${ASTRA_ENI_ROUTE_TABLES}" =~ ^[1-9][0-9]*(\ [1-9][0-9]*)*$ ]]; then
  echo "ASTRA_ENI_ROUTE_TABLES must be a space-separated list of positive integers." >&2
  exit 2
fi
read -r -a ROUTE_TABLES <<<"${ASTRA_ENI_ROUTE_TABLES}"
declare -A SEEN_ROUTE_TABLES=()
for ROUTE_TABLE in "${ROUTE_TABLES[@]}"; do
  if [[ -n "${SEEN_ROUTE_TABLES[${ROUTE_TABLE}]+present}" ]]; then
    echo "ASTRA_ENI_ROUTE_TABLES contains duplicate table ${ROUTE_TABLE}." >&2
    exit 2
  fi
  SEEN_ROUTE_TABLES["${ROUTE_TABLE}"]=1
done
table_is_managed() {
  local wanted="$1"
  local table
  for table in "${ROUTE_TABLES[@]}"; do
    [[ "${table}" == "${wanted}" ]] && return 0
  done
  return 1
}
if [[ "${ASTRA_DPDK_HUGE_DIR}" != /* ||
      "${ASTRA_DPDK_HUGE_DIR}" == / ||
      "/${ASTRA_DPDK_HUGE_DIR}/" == *"/../"* ]]; then
  echo "ASTRA_DPDK_HUGE_DIR must be a safe absolute directory." >&2
  exit 2
fi
for RECOVERY_ARTIFACT in \
  "${ASTRA_DPDK_STATE_FILE}" \
  "${ASTRA_DPDK_ROUTES_FILE}" \
  "${ASTRA_DPDK_RULES_FILE}"; do
  if [[ "${RECOVERY_ARTIFACT}" != /* ||
        "${RECOVERY_ARTIFACT}" == / ]]; then
    echo "DPDK recovery paths must be absolute file paths." >&2
    exit 2
  fi
  if [[ -e "${RECOVERY_ARTIFACT}" ]]; then
    echo "Existing DPDK recovery state must not be overwritten: ${RECOVERY_ARTIFACT}" >&2
    echo "Inspect it and run scripts/dpdk/dpdk_05_restore.sh before another bind." >&2
    exit 1
  fi
done

if [[ ! -e "/sys/class/net/${ASTRA_FEED_IFACE}" ]]; then
  echo "Feed interface does not exist: ${ASTRA_FEED_IFACE}" >&2
  echo "If it is already bound to vfio-pci, run scripts/dpdk/dpdk_04_run.sh instead." >&2
  exit 1
fi

ACTUAL_FEED_PCI="$(pci_for_iface "${ASTRA_FEED_IFACE}")"
if [[ "${ACTUAL_FEED_PCI}" != "${ASTRA_FEED_PCI}" ]]; then
  echo "Configured feed PCI ${ASTRA_FEED_PCI} does not match ${ASTRA_FEED_IFACE}: ${ACTUAL_FEED_PCI}" >&2
  exit 1
fi
ACTUAL_FEED_NUMA="$(<"/sys/class/net/${ASTRA_FEED_IFACE}/device/numa_node")"
if [[ "${ACTUAL_FEED_NUMA}" != "${ASTRA_FEED_NUMA}" ]]; then
  echo "Configured feed NUMA node ${ASTRA_FEED_NUMA} does not match ${ASTRA_FEED_IFACE}: ${ACTUAL_FEED_NUMA}" >&2
  exit 1
fi
ACTUAL_FEED_DRIVER="$(driver_for_pci "${ASTRA_FEED_PCI}" || true)"
if [[ "${ACTUAL_FEED_DRIVER}" != "${ASTRA_FEED_DRIVER}" ]]; then
  echo "Expected ${ASTRA_FEED_PCI} to use ${ASTRA_FEED_DRIVER}, found ${ACTUAL_FEED_DRIVER:-none}." >&2
  exit 1
fi
if ! FEED_ADDRESS_OUTPUT="$(
  ip -o -4 addr show dev "${ASTRA_FEED_IFACE}"
)"; then
  echo "Could not read IPv4 state for ${ASTRA_FEED_IFACE}." >&2
  exit 1
fi
mapfile -t FEED_CIDRS < <(
  awk 'NF >= 4 {print $4}' <<<"${FEED_ADDRESS_OUTPUT}"
)
if [[ "${#FEED_CIDRS[@]}" -ne 1 ]]; then
  echo "${ASTRA_FEED_IFACE} must have exactly one IPv4 address before DPDK binding." >&2
  echo "Found ${#FEED_CIDRS[@]}; no network state was changed." >&2
  exit 1
fi
SAVED_FEED_CIDR="${FEED_CIDRS[0]}"
if [[ "${SAVED_FEED_CIDR%/*}" != "${ASTRA_FEED_IP}" ]]; then
  echo "${ASTRA_FEED_IP} is not the sole address on ${ASTRA_FEED_IFACE}: ${SAVED_FEED_CIDR}" >&2
  exit 1
fi

protect_iface "${ASTRA_PROTECTED_IFACE}" "configured protected"
if [[ -n "${ASTRA_PROTECTED_IFACE}" ]]; then
  if [[ ! -e "/sys/class/net/${ASTRA_PROTECTED_IFACE}" ]]; then
    echo "Configured protected interface does not exist: ${ASTRA_PROTECTED_IFACE}" >&2
    exit 1
  fi
  ACTUAL_PROTECTED_PCI="$(pci_for_iface "${ASTRA_PROTECTED_IFACE}")"
  if [[ "${ACTUAL_PROTECTED_PCI}" != "${ASTRA_PROTECTED_PCI}" ]]; then
    echo "Configured protected PCI ${ASTRA_PROTECTED_PCI} does not match ${ASTRA_PROTECTED_IFACE}: ${ACTUAL_PROTECTED_PCI}" >&2
    exit 1
  fi
  PROTECTED_DRIVER_BEFORE="$(
    driver_for_pci "${ASTRA_PROTECTED_PCI}" || true
  )"
fi

if ! IPV4_ROUTE_OUTPUT="$(ip -o -4 route show table all)"; then
  echo "Could not inspect IPv4 routes; refusing to change the feed ENI." >&2
  exit 1
fi
if ! IPV6_ROUTE_OUTPUT="$(ip -o -6 route show table all)"; then
  echo "Could not inspect IPv6 routes; refusing to change the feed ENI." >&2
  exit 1
fi
mapfile -t DEFAULT_IFACES < <(
  {
    awk '$1 == "default" {
      table="main"
      for (i=1; i<=NF; ++i) if ($i=="table") table=$(i+1)
      if (table=="main" || table=="254")
        for (i=1; i<=NF; ++i) if ($i=="dev") print $(i+1)
    }' <<<"${IPV4_ROUTE_OUTPUT}"
    awk '$1 == "default" {
      table="main"
      for (i=1; i<=NF; ++i) if ($i=="table") table=$(i+1)
      if (table=="main" || table=="254")
        for (i=1; i<=NF; ++i) if ($i=="dev") print $(i+1)
    }' <<<"${IPV6_ROUTE_OUTPUT}"
  } | sort -u
)
for DEFAULT_IFACE in "${DEFAULT_IFACES[@]}"; do
  protect_iface "${DEFAULT_IFACE}" "main default-route"
done

if ! FEED_IPV6_ADDRESS_OUTPUT="$(
  ip -o -6 addr show dev "${ASTRA_FEED_IFACE}"
)"; then
  echo "Could not inspect IPv6 state for ${ASTRA_FEED_IFACE}." >&2
  exit 1
fi
while IFS= read -r ADDRESS_LINE; do
  [[ -n "${ADDRESS_LINE}" ]] || continue
  read -r -a ADDRESS_TOKENS <<<"${ADDRESS_LINE}"
  ADDRESS_SCOPE=""
  for ((TOKEN_INDEX=0; TOKEN_INDEX<${#ADDRESS_TOKENS[@]}; ++TOKEN_INDEX)); do
    if [[ "${ADDRESS_TOKENS[TOKEN_INDEX]}" == scope &&
          $((TOKEN_INDEX + 1)) -lt ${#ADDRESS_TOKENS[@]} ]]; then
      ADDRESS_SCOPE="${ADDRESS_TOKENS[TOKEN_INDEX + 1]}"
      break
    fi
  done
  if [[ "${ADDRESS_SCOPE}" != link ]]; then
    echo "${ASTRA_FEED_IFACE} has non-link-local IPv6 state that this workflow will not remove:" >&2
    echo "${ADDRESS_LINE}" >&2
    exit 1
  fi
done <<<"${FEED_IPV6_ADDRESS_OUTPUT}"

validate_feed_routes() {
  local family="$1"
  local route_output="$2"
  local route_line
  local route_table
  local route_protocol
  local route_uses_feed
  local token_index
  local -a route_tokens

  while IFS= read -r route_line; do
    [[ -n "${route_line}" ]] || continue
    read -r -a route_tokens <<<"${route_line}"
    route_table=main
    route_protocol=""
    route_uses_feed=0
    for ((token_index=0; token_index<${#route_tokens[@]}; ++token_index)); do
      if [[ "${route_tokens[token_index]}" == dev &&
            $((token_index + 1)) -lt ${#route_tokens[@]} &&
            "${route_tokens[token_index + 1]}" == "${ASTRA_FEED_IFACE}" ]]; then
        route_uses_feed=1
      elif [[ "${route_tokens[token_index]}" == table &&
              $((token_index + 1)) -lt ${#route_tokens[@]} ]]; then
        route_table="${route_tokens[token_index + 1]}"
      elif [[ "${route_tokens[token_index]}" == proto &&
              $((token_index + 1)) -lt ${#route_tokens[@]} ]]; then
        route_protocol="${route_tokens[token_index + 1]}"
      fi
    done
    [[ "${route_uses_feed}" -eq 1 ]] || continue

    if [[ "${family}" == 4 ]] && table_is_managed "${route_table}"; then
      continue
    fi
    if [[ "${route_protocol}" == kernel &&
          ( "${route_table}" == main || "${route_table}" == 254 ||
            "${route_table}" == local || "${route_table}" == 255 ) ]]; then
      continue
    fi

    echo "${ASTRA_FEED_IFACE} has an unsupported IPv${family} route outside the saved policy tables:" >&2
    echo "${route_line}" >&2
    exit 1
  done <<<"${route_output}"
}
validate_feed_routes 4 "${IPV4_ROUTE_OUTPUT}"
validate_feed_routes 6 "${IPV6_ROUTE_OUTPUT}"

if [[ -n "${SSH_CONNECTION:-}" ]]; then
  SSH_CLIENT_IP="${SSH_CONNECTION%% *}"
  if [[ "${SSH_CLIENT_IP}" == *:* ]]; then
    if ! SSH_ROUTE="$(ip -o -6 route get "${SSH_CLIENT_IP}")"; then
      echo "Could not resolve the IPv6 SSH route; refusing to bind." >&2
      exit 1
    fi
  else
    if ! SSH_ROUTE="$(ip -o -4 route get "${SSH_CLIENT_IP}")"; then
      echo "Could not resolve the IPv4 SSH route; refusing to bind." >&2
      exit 1
    fi
  fi
  SSH_IFACE="$(
    awk '{for (i=1; i<=NF; ++i) if ($i=="dev") {print $(i+1); exit}}' \
      <<<"${SSH_ROUTE}"
  )"
  if [[ -z "${SSH_IFACE}" ]]; then
    echo "The SSH route has no interface; refusing to bind." >&2
    exit 1
  fi
  protect_iface "${SSH_IFACE}" "SSH"
fi

if [[ ! -e "/sys/bus/pci/devices/${ASTRA_FEED_PCI}/iommu_group" ]]; then
  if [[ "${ALLOW_UNSAFE_NOIOMMU}" -eq 0 ]]; then
    cat >&2 <<EOF
No IOMMU group is available for ${ASTRA_FEED_PCI}.
Nothing was changed. Prefer an IOMMU-enabled host. On a dedicated, disposable
host only, rerun with --allow-unsafe-noiommu.
EOF
    exit 1
  fi
  USE_UNSAFE_NOIOMMU=1
elif [[ "${ALLOW_UNSAFE_NOIOMMU}" -eq 1 ]]; then
  echo "IOMMU is available; using normal VFIO isolation." >&2
fi

DPDK_DEVBIND="$(command -v dpdk-devbind.py || true)"
if [[ -z "${DPDK_DEVBIND}" &&
      -x /usr/share/dpdk/usertools/dpdk-devbind.py ]]; then
  DPDK_DEVBIND=/usr/share/dpdk/usertools/dpdk-devbind.py
fi
if [[ -z "${DPDK_DEVBIND}" ]]; then
  echo "dpdk-devbind.py was not found; run scripts/dpdk/dpdk_01_install.sh first." >&2
  exit 1
fi

echo "Feed interface: ${ASTRA_FEED_IFACE}"
echo "Feed address:   ${ASTRA_FEED_IP}"
echo "Feed PCI:       ${ASTRA_FEED_PCI}"
echo "Feed NUMA:      ${ASTRA_FEED_NUMA}"
echo "Protected NIC:  ${ASTRA_PROTECTED_IFACE} / ${ASTRA_PROTECTED_PCI}"

SAVED_ROUTE_RECORDS=()
for ROUTE_TABLE in "${ROUTE_TABLES[@]}"; do
  TABLE_HAS_ROUTES="$(
    awk -v table="${ROUTE_TABLE}" '
      {
        for (i=1; i<=NF; ++i) {
          if ($i == "table" && $(i+1) == table) {
            found=1
            exit
          }
        }
      }
      END {print found ? 1 : 0}
    ' <<<"${IPV4_ROUTE_OUTPUT}"
  )"
  ROUTE_TABLE_CONTENT=""
  if [[ "${TABLE_HAS_ROUTES}" -eq 1 ]]; then
    if ! ROUTE_TABLE_CONTENT="$(
      ip -o -4 route show table "${ROUTE_TABLE}"
    )"; then
      echo "Could not inspect route table ${ROUTE_TABLE}; refusing to bind." >&2
      exit 1
    fi
  fi

  while IFS= read -r ROUTE_LINE; do
    [[ -n "${ROUTE_LINE}" ]] || continue
    read -r -a ROUTE_TOKENS <<<"${ROUTE_LINE}"
    ROUTE_HAS_FEED_DEV=0
    for ((TOKEN_INDEX=0; TOKEN_INDEX<${#ROUTE_TOKENS[@]}; ++TOKEN_INDEX)); do
      if [[ "${ROUTE_TOKENS[TOKEN_INDEX]}" == dev ]]; then
        if [[ $((TOKEN_INDEX + 1)) -ge ${#ROUTE_TOKENS[@]} ||
              "${ROUTE_TOKENS[TOKEN_INDEX + 1]}" != "${ASTRA_FEED_IFACE}" ]]; then
          echo "Route table ${ROUTE_TABLE} is not dedicated to ${ASTRA_FEED_IFACE}:" >&2
          echo "${ROUTE_LINE}" >&2
          exit 1
        fi
        ROUTE_HAS_FEED_DEV=1
      fi
    done
    if [[ "${ROUTE_HAS_FEED_DEV}" -ne 1 ]]; then
      echo "Route table ${ROUTE_TABLE} is not dedicated to ${ASTRA_FEED_IFACE}:" >&2
      echo "${ROUTE_LINE}" >&2
      exit 1
    fi
    SAVED_ROUTE_RECORDS+=("${ROUTE_TABLE}"$'\t'"${ROUTE_LINE}")
  done <<<"${ROUTE_TABLE_CONTENT}"
done

if ! RULE_OUTPUT="$(ip -o rule show)"; then
  echo "Could not inspect policy rules; refusing to change the feed ENI." >&2
  exit 1
fi
SAVED_RULE_RECORDS=()
declare -A SAVED_RULE_PRIORITIES=()
while IFS= read -r RULE_LINE; do
  [[ -n "${RULE_LINE}" ]] || continue
  read -r -a RULE_TOKENS <<<"${RULE_LINE}"
  RULE_PRIORITY="${RULE_TOKENS[0]%:}"
  RULE_SOURCE=""
  RULE_TABLE=""
  for ((TOKEN_INDEX=0; TOKEN_INDEX<${#RULE_TOKENS[@]}; ++TOKEN_INDEX)); do
    if [[ "${RULE_TOKENS[TOKEN_INDEX]}" == from &&
          $((TOKEN_INDEX + 1)) -lt ${#RULE_TOKENS[@]} ]]; then
      RULE_SOURCE="${RULE_TOKENS[TOKEN_INDEX + 1]}"
    elif [[ ( "${RULE_TOKENS[TOKEN_INDEX]}" == lookup ||
              "${RULE_TOKENS[TOKEN_INDEX]}" == table ) &&
            $((TOKEN_INDEX + 1)) -lt ${#RULE_TOKENS[@]} ]]; then
      RULE_TABLE="${RULE_TOKENS[TOKEN_INDEX + 1]}"
    fi
  done
  RULE_SOURCE="${RULE_SOURCE%/32}"

  if table_is_managed "${RULE_TABLE}"; then
    if [[ "${RULE_SOURCE}" != "${ASTRA_FEED_IP}" ||
          ! "${RULE_PRIORITY}" =~ ^[1-9][0-9]*$ ||
          "${#RULE_TOKENS[@]}" -ne 5 ||
          "${RULE_TOKENS[1]}" != from ||
          ( "${RULE_TOKENS[3]}" != lookup &&
            "${RULE_TOKENS[3]}" != table ) ]]; then
      echo "Managed route table ${RULE_TABLE} has an unsupported or non-feed rule:" >&2
      echo "${RULE_LINE}" >&2
      exit 1
    fi
    if [[ -n "${SAVED_RULE_PRIORITIES[${RULE_PRIORITY}]+present}" ]]; then
      echo "Managed feed rules reuse priority ${RULE_PRIORITY}; exact restore is ambiguous." >&2
      exit 1
    fi
    RULE_PRIORITY_COUNT="$(
      awk -v priority="${RULE_PRIORITY}" '
        {
          current = $1
          sub(/:$/, "", current)
          if (current == priority) ++count
        }
        END {print count + 0}
      ' <<<"${RULE_OUTPUT}"
    )"
    if [[ "${RULE_PRIORITY_COUNT}" -ne 1 ]]; then
      echo "Policy-rule priority ${RULE_PRIORITY} is not unique; refusing to bind." >&2
      exit 1
    fi
    SAVED_RULE_PRIORITIES["${RULE_PRIORITY}"]=1
    SAVED_RULE_RECORDS+=(
      "${RULE_PRIORITY}"$'\t'"${RULE_SOURCE}"$'\t'"${RULE_TABLE}"
    )
  elif [[ "${RULE_SOURCE}" == "${ASTRA_FEED_IP}" ]]; then
    echo "Feed address ${ASTRA_FEED_IP} uses unmanaged route table ${RULE_TABLE}." >&2
    echo "Add it to ASTRA_ENI_ROUTE_TABLES before binding." >&2
    exit 1
  fi
done <<<"${RULE_OUTPUT}"

HUGEPAGE_NODE_DIR="/sys/devices/system/node/node${ASTRA_FEED_NUMA}/hugepages/hugepages-2048kB"
HUGEPAGE_COUNT_FILE="${HUGEPAGE_NODE_DIR}/nr_hugepages"
HUGEPAGE_FREE_FILE="${HUGEPAGE_NODE_DIR}/free_hugepages"
if [[ ! -e "${HUGEPAGE_COUNT_FILE}" ]]; then
  HUGEPAGE_COUNT_FILE=/proc/sys/vm/nr_hugepages
  HUGEPAGE_FREE_FILE=""
fi
SAVED_HUGEPAGES="$(<"${HUGEPAGE_COUNT_FILE}")"

SAVED_HUGE_MOUNTED=0
if mountpoint -q "${ASTRA_DPDK_HUGE_DIR}"; then
  SAVED_HUGE_MOUNTED=1
  HUGE_FSTYPE="$(
    findmnt -n -o FSTYPE --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  HUGE_OPTIONS="$(
    findmnt -n -o OPTIONS --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  if [[ "${HUGE_FSTYPE}" != hugetlbfs ||
        ",${HUGE_OPTIONS}," != *,pagesize=2M,* ]]; then
    echo "${ASTRA_DPDK_HUGE_DIR} is not a 2 MiB hugetlbfs mount." >&2
    exit 1
  fi
fi

SAVED_UNSAFE_NOIOMMU=0
if [[ -r /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
  SAVED_UNSAFE_NOIOMMU="$(
    tr 'YyNn' '1100' \
      </sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  )"
fi
BOUND_HUGEPAGES="${SAVED_HUGEPAGES}"
if (( BOUND_HUGEPAGES < ASTRA_DPDK_HUGE_PAGES )); then
  BOUND_HUGEPAGES="${ASTRA_DPDK_HUGE_PAGES}"
fi
BOUND_UNSAFE_NOIOMMU="${SAVED_UNSAFE_NOIOMMU}"
if [[ "${USE_UNSAFE_NOIOMMU}" -eq 1 ]]; then
  BOUND_UNSAFE_NOIOMMU=1
fi

astra_load_book_capacity
astra_require_capacity_admission check-memory

if [[ "${DRY_RUN}" -eq 0 ]]; then
  STATE_DIR="$(dirname "${ASTRA_DPDK_STATE_FILE}")"
  ROUTES_STATE_DIR="$(dirname "${ASTRA_DPDK_ROUTES_FILE}")"
  RULES_STATE_DIR="$(dirname "${ASTRA_DPDK_RULES_FILE}")"
  mkdir -p "${STATE_DIR}" "${ROUTES_STATE_DIR}" "${RULES_STATE_DIR}"
  umask 077
  STATE_TEMP="$(mktemp "${ASTRA_DPDK_STATE_FILE}.tmp.XXXXXX")"
  ROUTES_TEMP="$(mktemp "${ASTRA_DPDK_ROUTES_FILE}.tmp.XXXXXX")"
  RULES_TEMP="$(mktemp "${ASTRA_DPDK_RULES_FILE}.tmp.XXXXXX")"
  cleanup_state_temps() {
    rm -f -- "${STATE_TEMP}" "${ROUTES_TEMP}" "${RULES_TEMP}"
  }
  trap cleanup_state_temps EXIT
  {
    printf 'ASTRA_SAVED_FEED_IFACE=%q\n' "${ASTRA_FEED_IFACE}"
    printf 'ASTRA_SAVED_FEED_IP=%q\n' "${ASTRA_FEED_IP}"
    printf 'ASTRA_SAVED_FEED_CIDR=%q\n' "${SAVED_FEED_CIDR}"
    printf 'ASTRA_SAVED_FEED_PCI=%q\n' "${ASTRA_FEED_PCI}"
    printf 'ASTRA_SAVED_FEED_NUMA=%q\n' "${ASTRA_FEED_NUMA}"
    printf 'ASTRA_SAVED_FEED_DRIVER=%q\n' "${ASTRA_FEED_DRIVER}"
    printf 'ASTRA_SAVED_ROUTE_TABLES=%q\n' "${ASTRA_ENI_ROUTE_TABLES}"
    printf 'ASTRA_SAVED_HUGE_DIR=%q\n' "${ASTRA_DPDK_HUGE_DIR}"
    printf 'ASTRA_SAVED_HUGE_COUNT_FILE=%q\n' "${HUGEPAGE_COUNT_FILE}"
    printf 'ASTRA_SAVED_HUGEPAGES=%q\n' "${SAVED_HUGEPAGES}"
    printf 'ASTRA_BOUND_HUGEPAGES=%q\n' "${BOUND_HUGEPAGES}"
    printf 'ASTRA_SAVED_HUGE_MOUNTED=%q\n' "${SAVED_HUGE_MOUNTED}"
    printf 'ASTRA_SAVED_UNSAFE_NOIOMMU=%q\n' "${SAVED_UNSAFE_NOIOMMU}"
    printf 'ASTRA_BOUND_UNSAFE_NOIOMMU=%q\n' "${BOUND_UNSAFE_NOIOMMU}"
    printf 'ASTRA_BIND_COMPLETE=0\n'
  } >"${STATE_TEMP}"
  printf '%s\n' "${SAVED_ROUTE_RECORDS[@]}" >"${ROUTES_TEMP}"
  printf '%s\n' "${SAVED_RULE_RECORDS[@]}" >"${RULES_TEMP}"
  mv -f -- "${STATE_TEMP}" "${ASTRA_DPDK_STATE_FILE}"
  mv -f -- "${ROUTES_TEMP}" "${ASTRA_DPDK_ROUTES_FILE}"
  mv -f -- "${RULES_TEMP}" "${ASTRA_DPDK_RULES_FILE}"
  trap - EXIT
fi

ROLLBACK_ARMED=0
restore_feed_after_bind_failure() {
  local rollback_status=0

  ROLLBACK_ARMED=0
  trap - EXIT INT TERM HUP
  echo "Attempting feed-ENI rollback from ${ASTRA_DPDK_STATE_FILE}..." >&2
  "${ASTRA_REPO_ROOT}/scripts/dpdk/dpdk_05_restore.sh" || rollback_status=$?
  if [[ "${rollback_status}" -ne 0 ]]; then
    cat >&2 <<EOF
ROLLBACK FAILED (status ${rollback_status}).
Recovery state was retained at ${ASTRA_DPDK_STATE_FILE}.
Inspect the host, then rerun ./scripts/dpdk/dpdk_05_restore.sh with the same
ASTRA_FEED_* configuration.
EOF
  fi
  return "${rollback_status}"
}
rollback_on_exit() {
  local original_status=$?
  local rollback_status=0

  trap - EXIT INT TERM HUP
  if [[ "${ROLLBACK_ARMED}" -eq 1 && "${DRY_RUN}" -eq 0 ]]; then
    restore_feed_after_bind_failure || rollback_status=$?
    if [[ "${rollback_status}" -ne 0 ]]; then
      echo "The original bind failure status was ${original_status}." >&2
    fi
  fi
  exit "${original_status}"
}
trap rollback_on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
if [[ "${DRY_RUN}" -eq 0 ]]; then
  ROLLBACK_ARMED=1
fi

run_root mkdir -p "${ASTRA_DPDK_HUGE_DIR}"
if [[ "${SAVED_HUGE_MOUNTED}" -eq 0 ]]; then
  run_root mount -t hugetlbfs -o pagesize=2M nodev "${ASTRA_DPDK_HUGE_DIR}"
fi
if [[ "${DRY_RUN}" -eq 0 || "${SAVED_HUGE_MOUNTED}" -eq 1 ]]; then
  HUGE_FSTYPE="$(
    findmnt -n -o FSTYPE --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  HUGE_OPTIONS="$(
    findmnt -n -o OPTIONS --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  if [[ "${HUGE_FSTYPE}" != hugetlbfs ||
        ",${HUGE_OPTIONS}," != *,pagesize=2M,* ]]; then
    echo "${ASTRA_DPDK_HUGE_DIR} is not a 2 MiB hugetlbfs mount." >&2
    exit 1
  fi
fi

if (( SAVED_HUGEPAGES < ASTRA_DPDK_HUGE_PAGES )); then
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "+ set ${HUGEPAGE_COUNT_FILE} to ${ASTRA_DPDK_HUGE_PAGES}"
  else
    printf '%s\n' "${ASTRA_DPDK_HUGE_PAGES}" |
      run_root tee "${HUGEPAGE_COUNT_FILE}" >/dev/null
  fi
fi
if [[ "${DRY_RUN}" -eq 0 ]]; then
  CURRENT_HUGEPAGES="$(<"${HUGEPAGE_COUNT_FILE}")"
  if [[ "${CURRENT_HUGEPAGES}" != "${BOUND_HUGEPAGES}" ]]; then
    echo "Expected ${BOUND_HUGEPAGES} configured hugepages, found ${CURRENT_HUGEPAGES}." >&2
    exit 1
  fi
  if [[ -n "${HUGEPAGE_FREE_FILE}" && -r "${HUGEPAGE_FREE_FILE}" ]]; then
    FREE_HUGEPAGES="$(<"${HUGEPAGE_FREE_FILE}")"
  else
    FREE_HUGEPAGES="$(
      awk '/HugePages_Free:/ {print $2; exit}' /proc/meminfo
    )"
  fi
  if (( FREE_HUGEPAGES < ASTRA_DPDK_HUGE_PAGES )); then
    echo "Only ${FREE_HUGEPAGES} hugepages are free; ${ASTRA_DPDK_HUGE_PAGES} are required." >&2
    exit 1
  fi
fi

if [[ "${USE_UNSAFE_NOIOMMU}" -eq 1 ]]; then
  echo "WARNING: enabling unsafe VFIO no-IOMMU mode; DMA is not isolated." >&2
  run_root modprobe vfio enable_unsafe_noiommu_mode=1
  if [[ -e /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
    if [[ "${DRY_RUN}" -eq 1 ]]; then
      echo "+ set /sys/module/vfio/parameters/enable_unsafe_noiommu_mode to 1"
    else
      printf '1\n' |
        run_root tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode \
          >/dev/null
    fi
  fi
fi
run_root modprobe vfio-pci
if [[ "${DRY_RUN}" -eq 0 &&
      -r /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
  CURRENT_UNSAFE_NOIOMMU="$(
    tr 'YyNn' '1100' \
      </sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  )"
  if [[ "${CURRENT_UNSAFE_NOIOMMU}" != "${BOUND_UNSAFE_NOIOMMU}" ]]; then
    echo "VFIO no-IOMMU mode did not reach the requested state." >&2
    exit 1
  fi
elif [[ "${DRY_RUN}" -eq 0 &&
        "${BOUND_UNSAFE_NOIOMMU}" -ne 0 ]]; then
  echo "VFIO no-IOMMU mode is not readable after loading vfio-pci." >&2
  exit 1
fi

for RULE_RECORD in "${SAVED_RULE_RECORDS[@]}"; do
  IFS=$'\t' read -r RULE_PRIORITY RULE_SOURCE RULE_TABLE \
    <<<"${RULE_RECORD}"
  run_root ip rule del from "${RULE_SOURCE}/32" \
    table "${RULE_TABLE}" \
    priority "${RULE_PRIORITY}"
done
for ROUTE_TABLE in "${ROUTE_TABLES[@]}"; do
  TABLE_HAS_SAVED_ROUTES=0
  for ROUTE_RECORD in "${SAVED_ROUTE_RECORDS[@]}"; do
    IFS=$'\t' read -r SAVED_ROUTE_TABLE _ <<<"${ROUTE_RECORD}"
    if [[ "${SAVED_ROUTE_TABLE}" == "${ROUTE_TABLE}" ]]; then
      TABLE_HAS_SAVED_ROUTES=1
      break
    fi
  done
  if [[ "${TABLE_HAS_SAVED_ROUTES}" -eq 1 ]]; then
    run_root ip route flush table "${ROUTE_TABLE}"
  fi
done
run_root ip addr flush dev "${ASTRA_FEED_IFACE}"
run_root ip link set "${ASTRA_FEED_IFACE}" down

DEVBIND_ARGS=(--bind=vfio-pci "${ASTRA_FEED_PCI}")
if [[ "${USE_UNSAFE_NOIOMMU}" -eq 1 ]]; then
  DEVBIND_ARGS=(--noiommu-mode "${DEVBIND_ARGS[@]}")
fi
if ! run_root "${DPDK_DEVBIND}" "${DEVBIND_ARGS[@]}"; then
  echo "vfio-pci binding failed." >&2
  exit 1
fi

if [[ "${DRY_RUN}" -eq 0 ]]; then
  BOUND_DRIVER="$(driver_for_pci "${ASTRA_FEED_PCI}" || true)"
  if [[ "${BOUND_DRIVER}" != vfio-pci ]]; then
    echo "Binding failed: ${ASTRA_FEED_PCI} uses ${BOUND_DRIVER:-no driver}." >&2
    exit 1
  fi
  if [[ -e "/sys/class/net/${ASTRA_FEED_IFACE}" ]]; then
    echo "${ASTRA_FEED_IFACE} still exists after vfio-pci binding." >&2
    exit 1
  fi
  if [[ -n "${ASTRA_PROTECTED_IFACE}" ]]; then
    PROTECTED_DRIVER_AFTER="$(
      driver_for_pci "${ASTRA_PROTECTED_PCI}" || true
    )"
    if [[ "${PROTECTED_DRIVER_AFTER}" != "${PROTECTED_DRIVER_BEFORE}" ]]; then
      echo "Protected PCI ${ASTRA_PROTECTED_PCI} changed drivers unexpectedly." >&2
      exit 1
    fi
  fi
fi

run_root "${DPDK_DEVBIND}" --status
if [[ "${DRY_RUN}" -eq 1 ]]; then
  ROLLBACK_ARMED=0
  trap - EXIT INT TERM HUP
  echo "Bind dry run complete; no changes were applied."
  exit 0
fi

printf 'ASTRA_BIND_COMPLETE=1\n' >>"${ASTRA_DPDK_STATE_FILE}"
ROLLBACK_ARMED=0
trap - EXIT INT TERM HUP
echo "Step 3 complete: ${ASTRA_FEED_PCI} is ready for DPDK."
