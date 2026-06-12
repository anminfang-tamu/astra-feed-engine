#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
DATA_DIR="${DATA_DIR:-/data/itch/unzipped}"

RUN_APT=1
RUN_SUBMODULES=1
RUN_CONFIGURE=1
RUN_BUILD=1
RUN_TESTS=0
BUILD_TESTS=1
CREATE_DATA_DIR=1

usage() {
  cat <<'USAGE'
Usage: ./setup_ec2.sh [options]

Prepare an Ubuntu/Debian EC2 host for astra-feed-engine.

Options:
  --no-apt             Skip apt update/install.
  --no-submodules      Skip git submodule init.
  --no-configure       Skip CMake configure.
  --no-build           Skip CMake build.
  --no-tests           Configure without building tests.
  --run-tests          Run ctest after building.
  --no-data-dir        Do not create /data/itch/unzipped.
  --build-dir DIR      Override build directory.
  --build-type TYPE    CMake build type, default Release.
  --data-dir DIR       Data directory to create, default /data/itch/unzipped.
  -h, --help           Show this help.

Environment overrides:
  BUILD_DIR, BUILD_TYPE, DATA_DIR, CMAKE_GENERATOR
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-apt)
      RUN_APT=0
      ;;
    --no-submodules)
      RUN_SUBMODULES=0
      ;;
    --no-configure)
      RUN_CONFIGURE=0
      ;;
    --no-build)
      RUN_BUILD=0
      ;;
    --no-tests)
      BUILD_TESTS=0
      RUN_TESTS=0
      ;;
    --run-tests)
      BUILD_TESTS=1
      RUN_TESTS=1
      ;;
    --no-data-dir)
      CREATE_DATA_DIR=0
      ;;
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift
      ;;
    --build-type)
      BUILD_TYPE="${2:?missing value for --build-type}"
      shift
      ;;
    --data-dir)
      DATA_DIR="${2:?missing value for --data-dir}"
      shift
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

sudo_env() {
  sudo env DEBIAN_FRONTEND=noninteractive "$@"
}

install_apt_packages() {
  require_command sudo

  if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get was not found. This script expects Ubuntu or Debian." >&2
    exit 1
  fi

  local core_packages=(
    build-essential
    ca-certificates
    cmake
    curl
    gdb
    git
    libgtest-dev
    ninja-build
    pkg-config
    unzip
    zlib1g-dev
  )

  local benchmark_packages=(
    ethtool
    linux-tools-common
    linux-tools-generic
    numactl
    sysstat
  )

  log "Updating apt package index"
  sudo_env apt-get update

  log "Installing core build packages"
  sudo_env apt-get install -y --no-install-recommends "${core_packages[@]}"

  log "Installing optional benchmark/debug packages"
  if ! sudo_env apt-get install -y --no-install-recommends "${benchmark_packages[@]}"; then
    echo "Warning: optional benchmark/debug packages did not all install; continuing." >&2
  fi
}

create_data_dir() {
  log "Creating data directory: ${DATA_DIR}"
  sudo mkdir -p "${DATA_DIR}"

  local owner="${SUDO_USER:-${USER}}"
  if id "${owner}" >/dev/null 2>&1; then
    sudo chown "${owner}:${owner}" "${DATA_DIR}"
  fi
}

init_submodules() {
  if [[ ! -f "${ROOT_DIR}/.gitmodules" ]]; then
    log "No .gitmodules found; skipping submodule init"
    return
  fi

  log "Initializing git submodules"
  git -C "${ROOT_DIR}" submodule update --init --recursive
}

configure_project() {
  require_command cmake

  local generator="${CMAKE_GENERATOR:-Ninja}"
  if [[ "${generator}" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    generator="Unix Makefiles"
  fi

  local build_tests_flag=OFF
  if [[ "${BUILD_TESTS}" -eq 1 ]]; then
    build_tests_flag=ON
  fi

  log "Configuring ${BUILD_TYPE} build in ${BUILD_DIR}"
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "${generator}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DASTRA_BUILD_APPS=ON \
    -DASTRA_BUILD_TESTS="${build_tests_flag}" \
    -DASTRA_BUILD_BENCHMARKS=OFF
}

build_project() {
  require_command cmake

  local jobs
  jobs="$(nproc 2>/dev/null || echo 2)"

  log "Building astra-feed-engine with ${jobs} jobs"
  cmake --build "${BUILD_DIR}" -j"${jobs}"
}

run_tests() {
  require_command ctest

  log "Running tests"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
}

main() {
  if [[ "${RUN_APT}" -eq 1 ]]; then
    install_apt_packages
  fi

  if [[ "${CREATE_DATA_DIR}" -eq 1 ]]; then
    create_data_dir
  fi

  if [[ "${RUN_SUBMODULES}" -eq 1 ]]; then
    init_submodules
  fi

  if [[ "${RUN_CONFIGURE}" -eq 1 ]]; then
    configure_project
  fi

  if [[ "${RUN_BUILD}" -eq 1 ]]; then
    build_project
  fi

  if [[ "${RUN_TESTS}" -eq 1 ]]; then
    run_tests
  fi

  log "EC2 setup complete"
  echo "Build dir: ${BUILD_DIR}"
  echo "Data dir:  ${DATA_DIR}"
}

main "$@"
