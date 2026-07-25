#!/usr/bin/env bash
set -euo pipefail

DEVICE_NUMBER="${ASTRA_ENI_DEVICE_NUMBER:-1}"
IFACE="${ASTRA_FEED_IFACE:-}"
IP_CIDR="${ASTRA_ENI_IP_CIDR:-}"
SUBNET_CIDR="${ASTRA_ENI_SUBNET_CIDR:-}"
GATEWAY="${ASTRA_ENI_GATEWAY:-}"
ROUTE_TABLE="${ASTRA_ENI_ROUTE_TABLE:-1001}"
ROUTE_PRIORITY="${ASTRA_ENI_ROUTE_PRIORITY:-1001}"
MTU="${ASTRA_ENI_MTU:-}"
RX_RING="${ASTRA_ENI_RX_RING:-}"
TX_RING="${ASTRA_ENI_TX_RING:-}"
CONFIGURE_IPV4=1
CONFIGURE_POLICY_ROUTE=1
CONFIGURE_DEFAULT_ROUTE=1
DRY_RUN=0

IMDS_BASE="http://169.254.169.254/latest"
IMDS_TOKEN=""
IMDS_MAC=""
IMDS_INTERFACE_ID=""
IMDS_PRIVATE_IPV4=""
IMDS_PREFIX=""

usage() {
  cat <<'USAGE'
Usage:
  ./scripts/setup_secondary_eni.sh [options]

Configure the Linux side of an already-attached secondary EC2 ENI for feed RX.

Options:
  --iface IFACE          Interface name, for example ens6 or eth1.
                         Default: detect EC2 device-number 1.
  --device-number N      EC2 attachment device number to detect. Default: 1.
  --ip-cidr CIDR         Manually assign IPv4 CIDR, for example 10.0.2.25/24.
                         Default: read primary private IPv4 from EC2 metadata.
  --subnet-cidr CIDR     Subnet route CIDR for policy routing.
                         Default: read subnet CIDR from EC2 metadata.
  --gateway IP           Gateway for the per-source routing table.
                         Default: first address in the subnet CIDR.
  --table N              Policy routing table. Default: 1001.
  --priority N           Policy routing rule priority. Default: 1001.
  --mtu N                Set interface MTU.
  --rx-ring N            Set RX ring size with ethtool -G.
  --tx-ring N            Set TX ring size with ethtool -G.
  --no-ipv4              Bring the link up only; do not assign IPv4.
  --no-policy-route      Do not create source-based routing for the ENI IP.
  --no-default-route     Do not add default route to the ENI routing table.
  --dry-run              Print commands without applying host changes.
  -h, --help             Show this help.

Environment overrides:
  ASTRA_FEED_IFACE, ASTRA_ENI_DEVICE_NUMBER, ASTRA_ENI_IP_CIDR,
  ASTRA_ENI_SUBNET_CIDR, ASTRA_ENI_GATEWAY, ASTRA_ENI_ROUTE_TABLE,
  ASTRA_ENI_ROUTE_PRIORITY, ASTRA_ENI_MTU, ASTRA_ENI_RX_RING,
  ASTRA_ENI_TX_RING

Notes:
  This script does not create or attach the ENI in AWS. Attach the secondary ENI
  first, usually with EC2 device-index 1, then run this script on the instance.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --iface)
      IFACE="${2:?missing value for --iface}"
      shift
      ;;
    --device-number)
      DEVICE_NUMBER="${2:?missing value for --device-number}"
      shift
      ;;
    --ip-cidr)
      IP_CIDR="${2:?missing value for --ip-cidr}"
      shift
      ;;
    --subnet-cidr)
      SUBNET_CIDR="${2:?missing value for --subnet-cidr}"
      shift
      ;;
    --gateway)
      GATEWAY="${2:?missing value for --gateway}"
      shift
      ;;
    --table)
      ROUTE_TABLE="${2:?missing value for --table}"
      shift
      ;;
    --priority)
      ROUTE_PRIORITY="${2:?missing value for --priority}"
      shift
      ;;
    --mtu)
      MTU="${2:?missing value for --mtu}"
      shift
      ;;
    --rx-ring)
      RX_RING="${2:?missing value for --rx-ring}"
      shift
      ;;
    --tx-ring)
      TX_RING="${2:?missing value for --tx-ring}"
      shift
      ;;
    --no-ipv4)
      CONFIGURE_IPV4=0
      CONFIGURE_POLICY_ROUTE=0
      ;;
    --no-policy-route)
      CONFIGURE_POLICY_ROUTE=0
      ;;
    --no-default-route)
      CONFIGURE_DEFAULT_ROUTE=0
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

log() {
  printf '\n==> %s\n' "$*"
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

normalize_mac() {
  printf '%s' "${1%/}" | tr '[:upper:]' '[:lower:]'
}

run_sudo() {
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    printf '+ sudo'
    printf ' %q' "$@"
    printf '\n'
    return
  fi

  sudo "$@"
}

imds_init_token() {
  IMDS_TOKEN="$(
    curl -fsS --max-time 2 \
      -X PUT "${IMDS_BASE}/api/token" \
      -H "X-aws-ec2-metadata-token-ttl-seconds: 21600" 2>/dev/null || true
  )"
  [[ -n "${IMDS_TOKEN}" ]]
}

imds_get() {
  local path="$1"
  local url="${IMDS_BASE}/meta-data/${path}"

  [[ -n "${IMDS_TOKEN}" ]] || return 1
  curl -fsS --max-time 2 -H "X-aws-ec2-metadata-token: ${IMDS_TOKEN}" "${url}"
}

iface_for_mac() {
  local want_mac="${1%/}"
  local want_norm
  local net
  local got_mac

  want_norm="$(normalize_mac "${want_mac}")"

  for net in /sys/class/net/*; do
    [[ -e "${net}/address" ]] || continue
    got_mac="$(<"${net}/address")"
    if [[ "$(normalize_mac "${got_mac}")" == "${want_norm}" ]]; then
      basename "${net}"
      return 0
    fi
  done

  return 1
}

detect_default_iface() {
  ip route show default 2>/dev/null |
    awk '{for (i = 1; i <= NF; ++i) if ($i == "dev") {print $(i + 1); exit}}'
}

detect_fallback_secondary_iface() {
  local default_iface
  local candidate
  local found=""
  local net
  local count=0

  default_iface="$(detect_default_iface || true)"
  [[ -n "${default_iface}" ]] || return 1

  for net in /sys/class/net/*; do
    [[ -e "${net}" ]] || continue
    candidate="$(basename "${net}")"
    if [[ "${candidate}" != "lo" && "${candidate}" != "${default_iface}" ]]; then
      found="${candidate}"
      count=$((count + 1))
    fi
  done

  if [[ "${count}" -eq 1 ]]; then
    echo "${found}"
    return 0
  fi

  return 1
}

load_imds_for_device_number() {
  local mac
  local device_number

  imds_init_token || return 1

  while IFS= read -r mac; do
    mac="${mac%/}"
    [[ -n "${mac}" ]] || continue

    device_number="$(imds_get "network/interfaces/macs/${mac}/device-number" 2>/dev/null || true)"
    if [[ "${device_number}" == "${DEVICE_NUMBER}" ]]; then
      IMDS_MAC="${mac}"
      return 0
    fi
  done < <(imds_get "network/interfaces/macs/" 2>/dev/null || true)

  return 1
}

load_imds_for_iface() {
  local iface="$1"
  local iface_mac
  local mac

  [[ -r "/sys/class/net/${iface}/address" ]] || return 1
  iface_mac="$(<"/sys/class/net/${iface}/address")"

  imds_init_token || return 1

  while IFS= read -r mac; do
    mac="${mac%/}"
    [[ -n "${mac}" ]] || continue

    if [[ "$(normalize_mac "${mac}")" == "$(normalize_mac "${iface_mac}")" ]]; then
      IMDS_MAC="${mac}"
      return 0
    fi
  done < <(imds_get "network/interfaces/macs/" 2>/dev/null || true)

  return 1
}

populate_imds_network_values() {
  [[ -n "${IMDS_MAC}" ]] || return 1

  IMDS_INTERFACE_ID="$(imds_get "network/interfaces/macs/${IMDS_MAC}/interface-id" 2>/dev/null || true)"
  IMDS_PRIVATE_IPV4="$(
    imds_get "network/interfaces/macs/${IMDS_MAC}/local-ipv4s" 2>/dev/null |
      awk 'NF {print; exit}' || true
  )"
  SUBNET_CIDR="${SUBNET_CIDR:-$(imds_get "network/interfaces/macs/${IMDS_MAC}/subnet-ipv4-cidr-block" 2>/dev/null || true)}"

  if [[ -n "${SUBNET_CIDR}" && "${SUBNET_CIDR}" == */* ]]; then
    IMDS_PREFIX="${SUBNET_CIDR#*/}"
  fi
}

ip_to_int() {
  local ip="$1"
  local a b c d
  IFS=. read -r a b c d <<<"${ip}"
  echo $(( (a << 24) + (b << 16) + (c << 8) + d ))
}

int_to_ip() {
  local value="$1"
  printf '%d.%d.%d.%d\n' \
    "$(( (value >> 24) & 255 ))" \
    "$(( (value >> 16) & 255 ))" \
    "$(( (value >> 8) & 255 ))" \
    "$(( value & 255 ))"
}

gateway_from_cidr() {
  local cidr="$1"
  local base="${cidr%/*}"
  local prefix="${cidr#*/}"
  local ip_int
  local mask

  ip_int="$(ip_to_int "${base}")"
  mask=$(( (0xffffffff << (32 - prefix)) & 0xffffffff ))
  int_to_ip "$(( (ip_int & mask) + 1 ))"
}

validate_numeric() {
  local name="$1"
  local value="$2"

  if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
    echo "${name} must be a non-negative integer: ${value}" >&2
    exit 2
  fi
}

iface_has_ipv4() {
  local iface="$1"
  local ip="$2"

  ip -o -4 addr show dev "${iface}" |
    awk -v ip="${ip}" '{split($4, parts, "/"); if (parts[1] == ip) found = 1} END {exit found ? 0 : 1}'
}

policy_rule_exists() {
  local ip="$1"
  local table="$2"

  ip rule show |
    awk -v ip="${ip}" -v table="${table}" '
      $0 ~ ("from " ip "(/32)?") && $0 ~ ("lookup " table) { found = 1 }
      END { exit found ? 0 : 1 }
    '
}

main() {
  require_command ip
  require_command awk
  require_command curl
  require_command sudo
  require_command tr

  validate_numeric "--device-number" "${DEVICE_NUMBER}"
  validate_numeric "--table" "${ROUTE_TABLE}"
  validate_numeric "--priority" "${ROUTE_PRIORITY}"
  [[ -z "${MTU}" ]] || validate_numeric "--mtu" "${MTU}"
  [[ -z "${RX_RING}" ]] || validate_numeric "--rx-ring" "${RX_RING}"
  [[ -z "${TX_RING}" ]] || validate_numeric "--tx-ring" "${TX_RING}"

  local auto_selected=0
  local default_iface
  if [[ -z "${IFACE}" ]]; then
    auto_selected=1
    if [[ "${DEVICE_NUMBER}" -eq 0 ]]; then
      echo "Refusing to auto-select EC2 device-number 0 (the primary/management ENI)." >&2
      echo "Pass an explicit secondary --iface or use --device-number 1 or greater." >&2
      exit 1
    fi

    if load_imds_for_device_number; then
      IFACE="$(iface_for_mac "${IMDS_MAC}" || true)"
    else
      IFACE="$(detect_fallback_secondary_iface || true)"
    fi
  elif [[ "${CONFIGURE_IPV4}" -eq 1 ]]; then
    load_imds_for_iface "${IFACE}" || true
  fi

  if [[ -z "${IFACE}" ]]; then
    echo "Could not detect a secondary ENI. Pass --iface ens6 or --iface eth1." >&2
    exit 1
  fi

  if [[ ! -e "/sys/class/net/${IFACE}" ]]; then
    echo "Interface does not exist: ${IFACE}" >&2
    exit 1
  fi

  if [[ "${auto_selected}" -eq 1 ]]; then
    default_iface="$(detect_default_iface || true)"
    if [[ -n "${default_iface}" && "${IFACE}" == "${default_iface}" ]]; then
      echo "Refusing to auto-select the primary/default-route interface: ${IFACE}" >&2
      echo "Pass the secondary interface explicitly with --iface." >&2
      exit 1
    fi
  fi

  if [[ -n "${IMDS_MAC}" ]]; then
    populate_imds_network_values || true
  fi

  if [[ "${CONFIGURE_IPV4}" -eq 1 && -z "${IP_CIDR}" ]]; then
    if [[ -z "${IMDS_PRIVATE_IPV4}" || -z "${IMDS_PREFIX}" ]]; then
      echo "Could not read secondary ENI IPv4/CIDR from EC2 metadata." >&2
      echo "Pass --ip-cidr and --subnet-cidr, or use --no-ipv4." >&2
      exit 1
    fi
    IP_CIDR="${IMDS_PRIVATE_IPV4}/${IMDS_PREFIX}"
  fi

  local ip_addr=""
  if [[ -n "${IP_CIDR}" ]]; then
    ip_addr="${IP_CIDR%/*}"
  fi

  log "Configuring secondary ENI"
  echo "Interface: ${IFACE}"
  if [[ -n "${IMDS_INTERFACE_ID}" ]]; then
    echo "EC2 interface ID: ${IMDS_INTERFACE_ID}"
  fi
  if [[ -n "${IMDS_MAC}" ]]; then
    echo "MAC: ${IMDS_MAC}"
  fi
  if [[ -n "${IP_CIDR}" ]]; then
    echo "IPv4 CIDR: ${IP_CIDR}"
  fi
  if [[ -n "${SUBNET_CIDR}" ]]; then
    echo "Subnet CIDR: ${SUBNET_CIDR}"
  fi

  run_sudo ip link set dev "${IFACE}" up

  if [[ -n "${MTU}" ]]; then
    run_sudo ip link set dev "${IFACE}" mtu "${MTU}"
  fi

  if [[ -n "${RX_RING}" || -n "${TX_RING}" ]]; then
    require_command ethtool

    local ethtool_args=(-G "${IFACE}")
    if [[ -n "${RX_RING}" ]]; then
      ethtool_args+=(rx "${RX_RING}")
    fi
    if [[ -n "${TX_RING}" ]]; then
      ethtool_args+=(tx "${TX_RING}")
    fi

    run_sudo ethtool "${ethtool_args[@]}"
  fi

  if [[ "${CONFIGURE_IPV4}" -eq 1 ]]; then
    if iface_has_ipv4 "${IFACE}" "${ip_addr}"; then
      echo "IPv4 ${ip_addr} is already assigned to ${IFACE}; leaving it in place."
    else
      run_sudo ip addr add "${IP_CIDR}" dev "${IFACE}"
    fi
  fi

  if [[ "${CONFIGURE_POLICY_ROUTE}" -eq 1 ]]; then
    if [[ -z "${SUBNET_CIDR}" ]]; then
      echo "Cannot configure policy route without --subnet-cidr or EC2 metadata." >&2
      exit 1
    fi

    if [[ -z "${GATEWAY}" ]]; then
      GATEWAY="$(gateway_from_cidr "${SUBNET_CIDR}")"
    fi

    run_sudo ip route replace "${SUBNET_CIDR}" dev "${IFACE}" src "${ip_addr}" table "${ROUTE_TABLE}"
    if [[ "${CONFIGURE_DEFAULT_ROUTE}" -eq 1 ]]; then
      run_sudo ip route replace default via "${GATEWAY}" dev "${IFACE}" src "${ip_addr}" table "${ROUTE_TABLE}"
    fi

    if policy_rule_exists "${ip_addr}" "${ROUTE_TABLE}"; then
      echo "Policy rule for ${ip_addr} table ${ROUTE_TABLE} already exists."
    else
      run_sudo ip rule add from "${ip_addr}/32" table "${ROUTE_TABLE}" priority "${ROUTE_PRIORITY}"
    fi
  fi

  if [[ -r "/sys/class/net/${IFACE}/device/numa_node" ]]; then
    echo "Interface NUMA node: $(<"/sys/class/net/${IFACE}/device/numa_node")"
  fi

  log "Current interface state"
  ip -br addr show dev "${IFACE}" || true

  if [[ "${CONFIGURE_POLICY_ROUTE}" -eq 1 ]]; then
    log "Policy route state"
    ip rule show | awk -v table="${ROUTE_TABLE}" '$0 ~ ("lookup " table)'
    ip route show table "${ROUTE_TABLE}" || true
  fi

  cat <<EOF

Secondary ENI setup complete.

Use the secondary ENI private IP as the sender destination, and keep md_engine
bound to 0.0.0.0:

  ASTRA_BOOK_CAPACITY_PROFILE=<checksum-backed-profile> \\
  ASTRA_BOOK_PREFAULT=on \\
  ./scripts/run_engine_udp.sh 0.0.0.0 9000 0.0.0.0 9001

For a custom profile, also provide its canonical evidence file, SHA-256, and
the exact capacity/evidence/headroom environment documented in the runbook.

For NUMA reporting on this interface:

  cat /sys/class/net/${IFACE}/device/numa_node
EOF
}

main "$@"
