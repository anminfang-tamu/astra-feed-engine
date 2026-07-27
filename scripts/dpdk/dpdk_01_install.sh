#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dpdk_ec2_config.sh
source "${SCRIPT_DIR}/dpdk_ec2_config.sh"

usage() {
  cat <<'USAGE'
Usage: ./scripts/dpdk/dpdk_01_install.sh

Install the Ubuntu/Debian packages needed to build and run md_engine with
DPDK. This script does not configure a NIC or start the engine.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "apt-get was not found; this script expects Ubuntu or Debian." >&2
  exit 1
fi
if ! command -v sudo >/dev/null 2>&1 && [[ "${EUID}" -ne 0 ]]; then
  echo "sudo is required when this script is not run as root." >&2
  exit 1
fi

run_root() {
  if [[ "${EUID}" -eq 0 ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

echo "Installing build and DPDK packages..."
run_root env DEBIAN_FRONTEND=noninteractive apt-get update
run_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  cmake \
  ethtool \
  git \
  iproute2 \
  libdpdk-dev \
  ninja-build \
  numactl \
  pciutils \
  pkg-config \
  python3 \
  sudo \
  dpdk \
  dpdk-dev

echo
echo "Installed libdpdk:"
pkg-config --modversion libdpdk
pkg-config --libs libdpdk

DPDK_DEVBIND="$(command -v dpdk-devbind.py || true)"
if [[ -z "${DPDK_DEVBIND}" &&
      -x /usr/share/dpdk/usertools/dpdk-devbind.py ]]; then
  DPDK_DEVBIND=/usr/share/dpdk/usertools/dpdk-devbind.py
fi
if [[ -z "${DPDK_DEVBIND}" ]]; then
  echo "dpdk-devbind.py was not installed with the DPDK packages." >&2
  exit 1
fi

echo "DPDK device tool: ${DPDK_DEVBIND}"
echo "Step 1 complete."
