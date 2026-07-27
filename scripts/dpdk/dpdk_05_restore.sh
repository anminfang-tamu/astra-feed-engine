#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dpdk_ec2_config.sh
source "${SCRIPT_DIR}/dpdk_ec2_config.sh"

DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: ./scripts/dpdk/dpdk_05_restore.sh [--dry-run]

Return the feed PCI device to its saved Linux driver and restore the exact
IPv4 address, policy routes, policy rules, hugepage count, and VFIO safety
setting captured by dpdk_03_bind.sh.

Options:
  --dry-run   Print privileged mutations without applying them.
  -h, --help  Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
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
  local driver_path="/sys/bus/pci/devices/$1/driver"
  [[ -L "${driver_path}" ]] || return 1
  basename "$(readlink -f "${driver_path}")"
}

pci_for_iface() {
  local device_path="/sys/class/net/$1/device"
  [[ -e "${device_path}" ]] || return 1
  basename "$(readlink -f "${device_path}")"
}

protect_iface() {
  local iface="$1"
  local reason="$2"
  local pci

  [[ -n "${iface}" ]] || return
  pci="$(pci_for_iface "${iface}" || true)"
  if [[ "${iface}" == "${ASTRA_SAVED_FEED_IFACE}" ||
        ( -n "${pci}" && "${pci}" == "${ASTRA_SAVED_FEED_PCI}" ) ]]; then
    echo "Refusing to restore ${ASTRA_SAVED_FEED_PCI}: it is the ${reason} interface." >&2
    exit 1
  fi
}

rule_at_priority() {
  local priority="$1"
  ip -o rule show |
    awk -v priority="${priority}" '
      {
        current = $1
        sub(/:$/, "", current)
        if (current == priority) {
          print
          found = 1
        }
      }
      END {exit found ? 0 : 1}
    '
}

rule_matches() {
  local rule_line="$1"
  local expected_source="$2"
  local expected_table="$3"
  awk -v source="${expected_source}" -v table="${expected_table}" '
    {
      actual_source = ""
      actual_table = ""
      for (i=1; i<=NF; ++i) {
        if ($i == "from") actual_source = $(i+1)
        if ($i == "lookup" || $i == "table") actual_table = $(i+1)
      }
      sub(/\/32$/, "", actual_source)
      if (actual_source == source && actual_table == table) matched = 1
    }
    END {exit matched ? 0 : 1}
  ' <<<"${rule_line}"
}

table_is_saved() {
  local wanted="$1"
  [[ -n "${wanted}" &&
     -n "${SEEN_ROUTE_TABLES[${wanted}]+present}" ]]
}

managed_route_table_content() {
  local table="$1"
  local all_routes="$2"
  local table_has_routes

  table_has_routes="$(
    awk -v table="${table}" '
      {
        for (i=1; i<=NF; ++i) {
          if ($i == "table" && $(i+1) == table) {
            found=1
            exit
          }
        }
      }
      END {print found ? 1 : 0}
    ' <<<"${all_routes}"
  )"
  if [[ "${table_has_routes}" -eq 1 ]]; then
    ip -o -4 route show table "${table}"
  fi
}

expected_route_table_content() {
  local table="$1"
  awk -F $'\t' -v table="${table}" '
    $1 == table {
      sub(/^[^\t]*\t/, "")
      print
    }
  ' "${ASTRA_DPDK_ROUTES_FILE}" | sort
}

validate_managed_routes() {
  local require_all="$1"
  local all_routes
  local table
  local expected_routes
  local current_routes
  local route_line

  if ! all_routes="$(ip -o -4 route show table all)"; then
    echo "Could not inspect managed route tables." >&2
    return 1
  fi
  for table in "${SAVED_ROUTE_TABLES[@]}"; do
    if ! current_routes="$(
      managed_route_table_content "${table}" "${all_routes}"
    )"; then
      echo "Could not inspect saved route table ${table}." >&2
      return 1
    fi
    current_routes="$(sort <<<"${current_routes}")"
    expected_routes="$(expected_route_table_content "${table}")"
    while IFS= read -r route_line; do
      [[ -n "${route_line}" ]] || continue
      if ! grep -Fxq -- "${route_line}" <<<"${expected_routes}"; then
        echo "Route table ${table} changed after DPDK binding:" >&2
        echo "${route_line}" >&2
        return 1
      fi
    done <<<"${current_routes}"
    if [[ "${require_all}" -eq 1 &&
          "${current_routes}" != "${expected_routes}" ]]; then
      echo "Route table ${table} was not restored exactly." >&2
      return 1
    fi
  done
}

saved_rule_record_exists() {
  local wanted_priority="$1"
  local wanted_source="$2"
  local wanted_table="$3"
  local priority
  local source
  local table

  while IFS=$'\t' read -r priority source table; do
    [[ -n "${priority}" ]] || continue
    if [[ "${priority}" == "${wanted_priority}" &&
          "${source}" == "${wanted_source}" &&
          "${table}" == "${wanted_table}" ]]; then
      return 0
    fi
  done <"${ASTRA_DPDK_RULES_FILE}"
  return 1
}

validate_managed_rules() {
  local require_all="$1"
  local rule_output
  local rule_line
  local rule_priority
  local rule_source
  local rule_table
  local token_index
  local key
  local saved_priority
  local saved_source
  local saved_table
  local priority_count
  local -a rule_tokens
  local -A found_rules=()

  if ! rule_output="$(ip -o rule show)"; then
    echo "Could not inspect policy rules." >&2
    return 1
  fi
  while IFS= read -r rule_line; do
    [[ -n "${rule_line}" ]] || continue
    read -r -a rule_tokens <<<"${rule_line}"
    rule_priority="${rule_tokens[0]%:}"
    rule_source=""
    rule_table=""
    for ((token_index=0; token_index<${#rule_tokens[@]}; ++token_index)); do
      if [[ "${rule_tokens[token_index]}" == from &&
            $((token_index + 1)) -lt ${#rule_tokens[@]} ]]; then
        rule_source="${rule_tokens[token_index + 1]}"
      elif [[ ( "${rule_tokens[token_index]}" == lookup ||
                "${rule_tokens[token_index]}" == table ) &&
              $((token_index + 1)) -lt ${#rule_tokens[@]} ]]; then
        rule_table="${rule_tokens[token_index + 1]}"
      fi
    done
    rule_source="${rule_source%/32}"
    if ! table_is_saved "${rule_table}" &&
        [[ "${rule_source}" != "${ASTRA_SAVED_FEED_IP}" ]]; then
      continue
    fi
    if [[ ! "${rule_priority}" =~ ^[1-9][0-9]*$ ||
          "${#rule_tokens[@]}" -ne 5 ||
          "${rule_tokens[1]}" != from ||
          ( "${rule_tokens[3]}" != lookup &&
            "${rule_tokens[3]}" != table ) ]] ||
        ! saved_rule_record_exists \
          "${rule_priority}" "${rule_source}" "${rule_table}"; then
      echo "Policy rules changed after DPDK binding:" >&2
      echo "${rule_line}" >&2
      return 1
    fi
    key="${rule_priority}|${rule_source}|${rule_table}"
    if [[ -n "${found_rules[${key}]+present}" ]]; then
      echo "Policy rule ${rule_priority} is duplicated." >&2
      return 1
    fi
    found_rules["${key}"]=1
  done <<<"${rule_output}"

  while IFS=$'\t' read -r saved_priority saved_source saved_table; do
    [[ -n "${saved_priority}" ]] || continue
    key="${saved_priority}|${saved_source}|${saved_table}"
    priority_count="$(
      awk -v priority="${saved_priority}" '
        {
          current = $1
          sub(/:$/, "", current)
          if (current == priority) ++count
        }
        END {print count + 0}
      ' <<<"${rule_output}"
    )"
    if [[ "${priority_count}" -gt 1 ||
          ( "${priority_count}" -eq 1 &&
            -z "${found_rules[${key}]+present}" ) ]]; then
      echo "Policy-rule priority ${saved_priority} changed after DPDK binding." >&2
      return 1
    fi
    if [[ "${require_all}" -eq 1 &&
          -z "${found_rules[${key}]+present}" ]]; then
        echo "Policy rule ${saved_priority} was not restored." >&2
        return 1
    fi
  done <"${ASTRA_DPDK_RULES_FILE}"
}

validate_recovery_records() {
  local record
  local route_table
  local route_line
  local route_has_feed_dev
  local token_index
  local rule_priority
  local rule_source
  local rule_table
  local extra
  local -a route_tokens
  local -A seen_routes=()
  local -A seen_rule_priorities=()

  while IFS= read -r record; do
    [[ -n "${record}" ]] || continue
    if [[ "${record}" != *$'\t'* ]]; then
      echo "Recovery route record is malformed." >&2
      return 1
    fi
    route_table="${record%%$'\t'*}"
    route_line="${record#*$'\t'}"
    if [[ -z "${route_line}" || "${route_line}" == *$'\t'* ||
          ! "${route_table}" =~ ^[1-9][0-9]*$ ]] ||
        ! table_is_saved "${route_table}"; then
      echo "Recovery route record is invalid or uses an unsaved table." >&2
      return 1
    fi
    if [[ -n "${seen_routes[${record}]+present}" ]]; then
      echo "Recovery route record is duplicated." >&2
      return 1
    fi
    seen_routes["${record}"]=1
    read -r -a route_tokens <<<"${route_line}"
    route_has_feed_dev=0
    for ((token_index=0; token_index<${#route_tokens[@]}; ++token_index)); do
      if [[ "${route_tokens[token_index]}" == table ]]; then
        echo "Saved route unexpectedly embeds a table selector." >&2
        return 1
      fi
      if [[ "${route_tokens[token_index]}" == dev ]]; then
        if [[ $((token_index + 1)) -ge ${#route_tokens[@]} ||
              "${route_tokens[token_index + 1]}" != "${ASTRA_SAVED_FEED_IFACE}" ]]; then
          echo "Saved route is not dedicated to ${ASTRA_SAVED_FEED_IFACE}." >&2
          return 1
        fi
        route_has_feed_dev=1
      fi
    done
    if [[ "${route_has_feed_dev}" -ne 1 ]]; then
      echo "Saved route has no feed interface." >&2
      return 1
    fi
  done <"${ASTRA_DPDK_ROUTES_FILE}"

  while IFS=$'\t' read -r rule_priority rule_source rule_table extra; do
    [[ -n "${rule_priority}" ]] || continue
    if [[ -n "${extra}" ||
          ! "${rule_priority}" =~ ^[1-9][0-9]*$ ||
          "${rule_source}" != "${ASTRA_SAVED_FEED_IP}" ||
          ! "${rule_table}" =~ ^[1-9][0-9]*$ ]] ||
        ! table_is_saved "${rule_table}"; then
      echo "Recovery policy-rule record is invalid." >&2
      return 1
    fi
    if [[ -n "${seen_rule_priorities[${rule_priority}]+present}" ]]; then
      echo "Recovery policy-rule priority ${rule_priority} is duplicated." >&2
      return 1
    fi
    seen_rule_priorities["${rule_priority}"]=1
  done <"${ASTRA_DPDK_RULES_FILE}"
}

if [[ "$(uname -s)" != Linux ]]; then
  echo "DPDK device restoration is Linux-only." >&2
  exit 1
fi
for command_name in awk basename findmnt grep ip modprobe mountpoint readlink \
                    rm sort tee tr umount; do
  require_command "${command_name}"
done
if [[ "${EUID}" -ne 0 && "${DRY_RUN}" -eq 0 ]]; then
  require_command sudo
fi

for state_path in \
  "${ASTRA_DPDK_STATE_FILE}" \
  "${ASTRA_DPDK_ROUTES_FILE}" \
  "${ASTRA_DPDK_RULES_FILE}"; do
  if [[ ! -r "${state_path}" ]]; then
    echo "Missing DPDK recovery state: ${state_path}" >&2
    echo "Use the same configuration that ran dpdk_03_bind.sh." >&2
    exit 1
  fi
done

# shellcheck disable=SC1090
source "${ASTRA_DPDK_STATE_FILE}"

required_saved_values=(
  ASTRA_SAVED_FEED_IFACE
  ASTRA_SAVED_FEED_IP
  ASTRA_SAVED_FEED_CIDR
  ASTRA_SAVED_FEED_PCI
  ASTRA_SAVED_FEED_NUMA
  ASTRA_SAVED_FEED_DRIVER
  ASTRA_SAVED_ROUTE_TABLES
  ASTRA_SAVED_HUGE_DIR
  ASTRA_SAVED_HUGE_COUNT_FILE
  ASTRA_SAVED_HUGEPAGES
  ASTRA_BOUND_HUGEPAGES
  ASTRA_SAVED_HUGE_MOUNTED
  ASTRA_SAVED_UNSAFE_NOIOMMU
  ASTRA_BOUND_UNSAFE_NOIOMMU
  ASTRA_BIND_COMPLETE
)
for variable_name in "${required_saved_values[@]}"; do
  if [[ -z "${!variable_name-}" ]]; then
    echo "Recovery state is missing ${variable_name}." >&2
    exit 1
  fi
done

if [[ "${ASTRA_SAVED_FEED_IFACE}" != "${ASTRA_FEED_IFACE}" ||
      "${ASTRA_SAVED_FEED_IP}" != "${ASTRA_FEED_IP}" ||
      "${ASTRA_SAVED_FEED_PCI}" != "${ASTRA_FEED_PCI}" ||
      "${ASTRA_SAVED_FEED_NUMA}" != "${ASTRA_FEED_NUMA}" ||
      "${ASTRA_SAVED_FEED_DRIVER}" != "${ASTRA_FEED_DRIVER}" ||
      "${ASTRA_SAVED_ROUTE_TABLES}" != "${ASTRA_ENI_ROUTE_TABLES}" ||
      "${ASTRA_SAVED_HUGE_DIR}" != "${ASTRA_DPDK_HUGE_DIR}" ]]; then
  echo "Current EC2 DPDK configuration does not match the saved bind state." >&2
  echo "Restore with the same ASTRA_FEED_* values used for dpdk_03_bind.sh." >&2
  exit 1
fi
if [[ ! "${ASTRA_SAVED_FEED_IFACE}" =~ ^[[:alnum:]_.:-]+$ ||
      ! "${ASTRA_SAVED_FEED_PCI}" =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[[:xdigit:]]$ ||
      ! "${ASTRA_SAVED_FEED_DRIVER}" =~ ^[[:alnum:]_-]+$ ||
      ! "${ASTRA_SAVED_FEED_CIDR}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/[0-9]+$ ||
      ! "${ASTRA_SAVED_FEED_NUMA}" =~ ^[0-9]+$ ||
      ! "${ASTRA_SAVED_ROUTE_TABLES}" =~ ^[1-9][0-9]*(\ [1-9][0-9]*)*$ ||
      ! "${ASTRA_SAVED_HUGEPAGES}" =~ ^[0-9]+$ ||
      ! "${ASTRA_BOUND_HUGEPAGES}" =~ ^[0-9]+$ ||
      ! "${ASTRA_SAVED_HUGE_MOUNTED}" =~ ^[01]$ ||
      ! "${ASTRA_SAVED_UNSAFE_NOIOMMU}" =~ ^[01]$ ||
      ! "${ASTRA_BOUND_UNSAFE_NOIOMMU}" =~ ^[01]$ ||
      ! "${ASTRA_BIND_COMPLETE}" =~ ^[01]$ ]]; then
  echo "Recovery state contains an invalid value." >&2
  exit 1
fi
if (( ASTRA_BOUND_HUGEPAGES < ASTRA_SAVED_HUGEPAGES )); then
  echo "Recovery state has an invalid bound hugepage count." >&2
  exit 1
fi
read -r -a SAVED_ROUTE_TABLES <<<"${ASTRA_SAVED_ROUTE_TABLES}"
declare -A SEEN_ROUTE_TABLES=()
for ROUTE_TABLE in "${SAVED_ROUTE_TABLES[@]}"; do
  if [[ -n "${SEEN_ROUTE_TABLES[${ROUTE_TABLE}]+present}" ]]; then
    echo "Recovery state contains duplicate route table ${ROUTE_TABLE}." >&2
    exit 1
  fi
  SEEN_ROUTE_TABLES["${ROUTE_TABLE}"]=1
done
validate_recovery_records
EXPECTED_HUGE_COUNT_FILE="/sys/devices/system/node/node${ASTRA_SAVED_FEED_NUMA}/hugepages/hugepages-2048kB/nr_hugepages"
if [[ "${ASTRA_SAVED_HUGE_COUNT_FILE}" != "${EXPECTED_HUGE_COUNT_FILE}" &&
      "${ASTRA_SAVED_HUGE_COUNT_FILE}" != /proc/sys/vm/nr_hugepages ]]; then
  echo "Recovery state contains an unexpected hugepage control path." >&2
  exit 1
fi
if [[ "${ASTRA_SAVED_FEED_PCI}" == "${ASTRA_PROTECTED_PCI}" ]]; then
  echo "Saved feed PCI matches the configured protected PCI; refusing restore." >&2
  exit 1
fi

if ! IPV4_ROUTE_OUTPUT="$(ip -o -4 route show table all)"; then
  echo "Could not inspect IPv4 routes; refusing restore." >&2
  exit 1
fi
if ! IPV6_ROUTE_OUTPUT="$(ip -o -6 route show table all)"; then
  echo "Could not inspect IPv6 routes; refusing restore." >&2
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

if [[ -n "${SSH_CONNECTION:-}" ]]; then
  SSH_CLIENT_IP="${SSH_CONNECTION%% *}"
  if [[ "${SSH_CLIENT_IP}" == *:* ]]; then
    SSH_ROUTE="$(ip -o -6 route get "${SSH_CLIENT_IP}")"
  else
    SSH_ROUTE="$(ip -o -4 route get "${SSH_CLIENT_IP}")"
  fi
  SSH_IFACE="$(
    awk '{for (i=1; i<=NF; ++i) if ($i=="dev") {print $(i+1); exit}}' \
      <<<"${SSH_ROUTE}"
  )"
  [[ -n "${SSH_IFACE}" ]] || {
    echo "The SSH route has no interface; refusing restore." >&2
    exit 1
  }
  protect_iface "${SSH_IFACE}" "SSH"
fi

DPDK_DEVBIND="$(command -v dpdk-devbind.py || true)"
if [[ -z "${DPDK_DEVBIND}" &&
      -x /usr/share/dpdk/usertools/dpdk-devbind.py ]]; then
  DPDK_DEVBIND=/usr/share/dpdk/usertools/dpdk-devbind.py
fi
if [[ -z "${DPDK_DEVBIND}" ]]; then
  echo "dpdk-devbind.py was not found." >&2
  exit 1
fi

if [[ ! -e "${ASTRA_SAVED_HUGE_COUNT_FILE}" ]]; then
  echo "Hugepage control path disappeared: ${ASTRA_SAVED_HUGE_COUNT_FILE}" >&2
  exit 1
fi
CURRENT_HUGEPAGES="$(<"${ASTRA_SAVED_HUGE_COUNT_FILE}")"
if [[ "${CURRENT_HUGEPAGES}" != "${ASTRA_SAVED_HUGEPAGES}" &&
      "${CURRENT_HUGEPAGES}" != "${ASTRA_BOUND_HUGEPAGES}" ]]; then
  if [[ "${ASTRA_BIND_COMPLETE}" -eq 1 ||
        "${CURRENT_HUGEPAGES}" -lt "${ASTRA_SAVED_HUGEPAGES}" ||
        "${CURRENT_HUGEPAGES}" -gt "${ASTRA_BOUND_HUGEPAGES}" ]]; then
    echo "Hugepage count changed after DPDK binding; refusing to overwrite it." >&2
    exit 1
  fi
fi

if mountpoint -q "${ASTRA_DPDK_HUGE_DIR}"; then
  CURRENT_HUGE_FSTYPE="$(
    findmnt -n -o FSTYPE --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  CURRENT_HUGE_OPTIONS="$(
    findmnt -n -o OPTIONS --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  if [[ "${CURRENT_HUGE_FSTYPE}" != hugetlbfs ||
        ",${CURRENT_HUGE_OPTIONS}," != *,pagesize=2M,* ]]; then
    echo "Hugepage mount changed after DPDK binding; refusing restore." >&2
    exit 1
  fi
elif [[ "${ASTRA_SAVED_HUGE_MOUNTED}" -eq 1 ]]; then
  echo "The pre-existing hugepage mount disappeared; refusing restore." >&2
  exit 1
fi

if [[ -r /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
  CURRENT_UNSAFE_NOIOMMU="$(
    tr 'YyNn' '1100' \
      </sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  )"
  if [[ "${CURRENT_UNSAFE_NOIOMMU}" != "${ASTRA_SAVED_UNSAFE_NOIOMMU}" &&
        "${CURRENT_UNSAFE_NOIOMMU}" != "${ASTRA_BOUND_UNSAFE_NOIOMMU}" ]]; then
    echo "VFIO no-IOMMU mode changed after binding; refusing restore." >&2
    exit 1
  fi
elif [[ "${ASTRA_BOUND_UNSAFE_NOIOMMU}" -ne 0 ||
        "${ASTRA_SAVED_UNSAFE_NOIOMMU}" -ne 0 ]]; then
  echo "VFIO no-IOMMU state is no longer readable; refusing restore." >&2
  exit 1
fi

validate_managed_routes 0
validate_managed_rules 0

CURRENT_DRIVER="$(driver_for_pci "${ASTRA_SAVED_FEED_PCI}" || true)"
case "${CURRENT_DRIVER}" in
  "${ASTRA_SAVED_FEED_DRIVER}")
    ;;
  vfio-pci|"")
    run_root modprobe "${ASTRA_SAVED_FEED_DRIVER}"
    run_root "${DPDK_DEVBIND}" \
      --bind="${ASTRA_SAVED_FEED_DRIVER}" \
      "${ASTRA_SAVED_FEED_PCI}"
    ;;
  *)
    echo "${ASTRA_SAVED_FEED_PCI} now uses unexpected driver ${CURRENT_DRIVER}; refusing restore." >&2
    exit 1
    ;;
esac

if command -v udevadm >/dev/null 2>&1; then
  run_root udevadm settle
fi

if [[ "${DRY_RUN}" -eq 0 ]]; then
  RESTORED_DRIVER="$(
    driver_for_pci "${ASTRA_SAVED_FEED_PCI}" || true
  )"
  if [[ "${RESTORED_DRIVER}" != "${ASTRA_SAVED_FEED_DRIVER}" ]]; then
    echo "Restore failed: ${ASTRA_SAVED_FEED_PCI} uses ${RESTORED_DRIVER:-no driver}." >&2
    exit 1
  fi
  if [[ ! -e "/sys/class/net/${ASTRA_SAVED_FEED_IFACE}" ]]; then
    echo "${ASTRA_SAVED_FEED_IFACE} did not return after driver binding." >&2
    exit 1
  fi
  RESTORED_PCI="$(pci_for_iface "${ASTRA_SAVED_FEED_IFACE}")"
  if [[ "${RESTORED_PCI}" != "${ASTRA_SAVED_FEED_PCI}" ]]; then
    echo "${ASTRA_SAVED_FEED_IFACE} maps to ${RESTORED_PCI}, not ${ASTRA_SAVED_FEED_PCI}." >&2
    exit 1
  fi
fi

run_root ip link set "${ASTRA_SAVED_FEED_IFACE}" up
if [[ "${DRY_RUN}" -eq 0 ]]; then
  RESTORED_ADDRESS_OUTPUT="$(
    ip -o -4 addr show dev "${ASTRA_SAVED_FEED_IFACE}"
  )"
  mapfile -t RESTORED_CIDRS < <(
    awk 'NF >= 4 {print $4}' <<<"${RESTORED_ADDRESS_OUTPUT}"
  )
  if [[ "${#RESTORED_CIDRS[@]}" -gt 1 ||
        ( "${#RESTORED_CIDRS[@]}" -eq 1 &&
          "${RESTORED_CIDRS[0]}" != "${ASTRA_SAVED_FEED_CIDR}" ) ]]; then
    echo "${ASTRA_SAVED_FEED_IFACE} returned with unexpected IPv4 state." >&2
    exit 1
  fi
  if [[ "${#RESTORED_CIDRS[@]}" -eq 0 ]]; then
    run_root ip addr add "${ASTRA_SAVED_FEED_CIDR}" \
      dev "${ASTRA_SAVED_FEED_IFACE}"
  fi
else
  run_root ip addr replace "${ASTRA_SAVED_FEED_CIDR}" \
    dev "${ASTRA_SAVED_FEED_IFACE}"
fi

while IFS=$'\t' read -r ROUTE_TABLE ROUTE_LINE; do
  [[ -n "${ROUTE_TABLE}" && -n "${ROUTE_LINE}" ]] || continue
  [[ "${ROUTE_TABLE}" =~ ^[1-9][0-9]*$ ]] || {
    echo "Saved route table is invalid: ${ROUTE_TABLE}" >&2
    exit 1
  }
  read -r -a ROUTE_ARGS <<<"${ROUTE_LINE}"
  ROUTE_HAS_FEED_DEV=0
  for ((TOKEN_INDEX=0; TOKEN_INDEX<${#ROUTE_ARGS[@]}; ++TOKEN_INDEX)); do
    if [[ "${ROUTE_ARGS[TOKEN_INDEX]}" == dev ]]; then
      if [[ $((TOKEN_INDEX + 1)) -ge ${#ROUTE_ARGS[@]} ||
            "${ROUTE_ARGS[TOKEN_INDEX + 1]}" != "${ASTRA_SAVED_FEED_IFACE}" ]]; then
        echo "Saved route does not belong only to ${ASTRA_SAVED_FEED_IFACE}: ${ROUTE_LINE}" >&2
        exit 1
      fi
      ROUTE_HAS_FEED_DEV=1
    fi
  done
  if [[ "${ROUTE_HAS_FEED_DEV}" -ne 1 ]]; then
    echo "Saved route does not belong to ${ASTRA_SAVED_FEED_IFACE}: ${ROUTE_LINE}" >&2
    exit 1
  fi
  run_root ip route replace table "${ROUTE_TABLE}" "${ROUTE_ARGS[@]}"
done <"${ASTRA_DPDK_ROUTES_FILE}"

while IFS=$'\t' read -r RULE_PRIORITY RULE_SOURCE RULE_TABLE; do
  [[ -n "${RULE_PRIORITY}" ]] || continue
  if [[ ! "${RULE_PRIORITY}" =~ ^[1-9][0-9]*$ ||
        "${RULE_SOURCE}" != "${ASTRA_SAVED_FEED_IP}" ||
        ! "${RULE_TABLE}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Saved policy rule is invalid." >&2
    exit 1
  fi
  EXISTING_RULE="$(rule_at_priority "${RULE_PRIORITY}" || true)"
  if [[ -n "${EXISTING_RULE}" ]]; then
    if ! rule_matches "${EXISTING_RULE}" \
        "${RULE_SOURCE}" "${RULE_TABLE}"; then
      echo "Policy-rule priority ${RULE_PRIORITY} is occupied by another rule." >&2
      exit 1
    fi
  else
    run_root ip rule add from "${RULE_SOURCE}/32" \
      table "${RULE_TABLE}" \
      priority "${RULE_PRIORITY}"
  fi
done <"${ASTRA_DPDK_RULES_FILE}"

if [[ -r /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
  CURRENT_UNSAFE_NOIOMMU="$(
    tr 'YyNn' '1100' \
      </sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  )"
  if [[ "${CURRENT_UNSAFE_NOIOMMU}" != "${ASTRA_SAVED_UNSAFE_NOIOMMU}" ]]; then
    if [[ "${DRY_RUN}" -eq 1 ]]; then
      echo "+ set /sys/module/vfio/parameters/enable_unsafe_noiommu_mode to ${ASTRA_SAVED_UNSAFE_NOIOMMU}"
    else
      printf '%s\n' "${ASTRA_SAVED_UNSAFE_NOIOMMU}" |
        run_root tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode \
          >/dev/null
    fi
  fi
fi

if [[ "${ASTRA_SAVED_HUGE_MOUNTED}" -eq 0 ]] &&
    mountpoint -q "${ASTRA_DPDK_HUGE_DIR}"; then
  HUGE_FSTYPE="$(
    findmnt -n -o FSTYPE --target "${ASTRA_DPDK_HUGE_DIR}"
  )"
  if [[ "${HUGE_FSTYPE}" != hugetlbfs ]]; then
    echo "Refusing to unmount non-hugetlbfs path ${ASTRA_DPDK_HUGE_DIR}." >&2
    exit 1
  fi
  run_root umount "${ASTRA_DPDK_HUGE_DIR}"
fi

if [[ -e "${ASTRA_SAVED_HUGE_COUNT_FILE}" ]]; then
  CURRENT_HUGEPAGES="$(<"${ASTRA_SAVED_HUGE_COUNT_FILE}")"
  if [[ "${CURRENT_HUGEPAGES}" != "${ASTRA_SAVED_HUGEPAGES}" ]]; then
    if [[ "${DRY_RUN}" -eq 1 ]]; then
      echo "+ set ${ASTRA_SAVED_HUGE_COUNT_FILE} to ${ASTRA_SAVED_HUGEPAGES}"
    else
      printf '%s\n' "${ASTRA_SAVED_HUGEPAGES}" |
        run_root tee "${ASTRA_SAVED_HUGE_COUNT_FILE}" >/dev/null
    fi
  fi
fi

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "Restore dry run complete; no changes were applied."
  exit 0
fi

FINAL_CIDR="$(
  ip -o -4 addr show dev "${ASTRA_SAVED_FEED_IFACE}" |
    awk 'NF >= 4 {print $4; exit}'
)"
if [[ "${FINAL_CIDR}" != "${ASTRA_SAVED_FEED_CIDR}" ]]; then
  echo "Address restore failed: expected ${ASTRA_SAVED_FEED_CIDR}, found ${FINAL_CIDR:-none}." >&2
  exit 1
fi
validate_managed_routes 1
validate_managed_rules 1
if [[ -e "${ASTRA_SAVED_HUGE_COUNT_FILE}" ]]; then
  FINAL_HUGEPAGES="$(<"${ASTRA_SAVED_HUGE_COUNT_FILE}")"
  if [[ "${FINAL_HUGEPAGES}" != "${ASTRA_SAVED_HUGEPAGES}" ]]; then
    echo "Hugepage restore failed: expected ${ASTRA_SAVED_HUGEPAGES}, found ${FINAL_HUGEPAGES}." >&2
    exit 1
  fi
fi
if [[ -r /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
  FINAL_UNSAFE_NOIOMMU="$(
    tr 'YyNn' '1100' \
      </sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  )"
  if [[ "${FINAL_UNSAFE_NOIOMMU}" != "${ASTRA_SAVED_UNSAFE_NOIOMMU}" ]]; then
    echo "VFIO no-IOMMU setting restore failed." >&2
    exit 1
  fi
fi

"${DPDK_DEVBIND}" --status
rm -f -- \
  "${ASTRA_DPDK_STATE_FILE}" \
  "${ASTRA_DPDK_ROUTES_FILE}" \
  "${ASTRA_DPDK_RULES_FILE}"
echo "Step 5 complete: ${ASTRA_SAVED_FEED_IFACE} is back under Linux control."
