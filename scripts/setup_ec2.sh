#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
DATA_DIR="${DATA_DIR:-${ROOT_DIR}/data/itch/unzipped}"

RUN_APT=1
RUN_SUBMODULES=1
RUN_CONFIGURE=1
RUN_BUILD=1
RUN_TESTS=0
BUILD_TESTS=1
BUILD_BENCHMARKS=1
CREATE_DATA_DIR=1

usage() {
  cat <<'USAGE'
Usage: scripts/setup_ec2.sh [options]

Prepare an Ubuntu/Debian EC2 host for astra-feed-engine.

Options:
  --no-apt             Skip apt update/install.
  --no-submodules      Skip git submodule init.
  --no-configure       Skip CMake configure.
  --no-build           Skip CMake build.
  --no-tests           Configure without building tests.
  --no-benchmarks      Configure without benchmark binaries. Benchmarks are
                       built by default for the EC2 acceptance workflow.
  --run-tests          Run ctest after building.
  --no-data-dir        Do not create the trace directory.
  --build-dir DIR      Override build directory.
  --build-type TYPE    CMake build type, default Release.
  --data-dir DIR       Data directory to create, default
                       <repository>/data/itch/unzipped.
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
    --no-benchmarks)
      BUILD_BENCHMARKS=0
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
    binutils
    build-essential
    ca-certificates
    cmake
    coreutils
    curl
    findutils
    gawk
    gdb
    git
    grep
    libgtest-dev
    ninja-build
    pkg-config
    procps
    tar
    unzip
    zlib1g-dev
  )

  local acceptance_packages=(
    numactl
    python3
    util-linux
  )

  local optional_diagnostic_packages=(
    ethtool
    sysstat
  )

  log "Updating apt package index"
  sudo_env apt-get update

  log "Installing core build packages"
  sudo_env apt-get install -y --no-install-recommends "${core_packages[@]}"

  if [[ "${BUILD_BENCHMARKS}" -eq 1 ]]; then
    log "Installing required acceptance packages"
    sudo_env apt-get install -y --no-install-recommends \
      "${acceptance_packages[@]}"

    local kernel_perf_package="linux-tools-$(uname -r)"
    local perf_package=""
    if apt-cache show "${kernel_perf_package}" >/dev/null 2>&1; then
      perf_package="${kernel_perf_package}"
    elif apt-cache show linux-perf >/dev/null 2>&1; then
      perf_package=linux-perf
    elif apt-cache show linux-tools-generic >/dev/null 2>&1; then
      perf_package=linux-tools-generic
    else
      echo "No packaged perf implementation was found for this kernel." >&2
      echo "Install a kernel-matched perf package, then rerun with --no-apt." >&2
      exit 1
    fi
    log "Installing required perf package: ${perf_package}"
    sudo_env apt-get install -y --no-install-recommends "${perf_package}"

    log "Installing optional host-diagnostic packages"
    if ! sudo_env apt-get install -y --no-install-recommends \
         "${optional_diagnostic_packages[@]}"; then
      echo "Warning: optional host-diagnostic packages did not all install; continuing." >&2
    fi
  fi
}

create_data_dir() {
  log "Creating data directory: ${DATA_DIR}"
  if mkdir -p -- "${DATA_DIR}" 2>/dev/null; then
    return
  fi

  require_command sudo
  sudo mkdir -p -- "${DATA_DIR}"
  local owner="${SUDO_USER:-${USER:-$(id -un)}}"
  local group=""
  if id "${owner}" >/dev/null 2>&1; then
    group="$(id -gn "${owner}")"
    sudo chown "${owner}:${group}" "${DATA_DIR}"
  fi
}

verify_acceptance_tools() {
  if [[ "${BUILD_BENCHMARKS}" -ne 1 ]]; then
    return
  fi

  log "Verifying required acceptance commands"
  local command=""
  for command in cmake cmp env find git lscpu mktemp numactl objdump perf \
                 python3 readlink sha256sum tar taskset xargs; do
    require_command "${command}"
  done
  if ! perf --version >/dev/null 2>&1; then
    echo "perf is installed but cannot run for kernel $(uname -r)." >&2
    echo "Install the matching kernel tools package before acceptance." >&2
    exit 1
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

  if [[ ! -f "${ROOT_DIR}/CMakeLists.txt" ]]; then
    echo "CMakeLists.txt was not found at repo root: ${ROOT_DIR}" >&2
    exit 1
  fi

  local generator="${CMAKE_GENERATOR:-Ninja}"
  if [[ "${generator}" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    generator="Unix Makefiles"
  fi

  local build_tests_flag=OFF
  if [[ "${BUILD_TESTS}" -eq 1 ]]; then
    build_tests_flag=ON
  fi
  local build_benchmarks_flag=OFF
  if [[ "${BUILD_BENCHMARKS}" -eq 1 ]]; then
    build_benchmarks_flag=ON
  fi

  log "Configuring ${BUILD_TYPE} build in ${BUILD_DIR}"
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "${generator}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DASTRA_BUILD_APPS=ON \
    -DASTRA_BUILD_TESTS="${build_tests_flag}" \
    -DASTRA_BUILD_BENCHMARKS="${build_benchmarks_flag}"
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

  verify_acceptance_tools

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
  if [[ "${BUILD_BENCHMARKS}" -eq 1 ]]; then
    cat <<'NOTICE'

Acceptance software is installed, but host policy is intentionally not changed.
Before a live run, select an x86_64 CPU/NUMA node and manually verify CPU domain
isolation, performance governor (when exposed), disabled swap, THP always or
madvise, perf permissions, and single-node/cgroup memory headroom. The
run_order_book_acceptance.sh preflight is authoritative and fails closed.
NOTICE
  fi
}

main "$@"
