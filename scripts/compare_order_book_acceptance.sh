#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "acceptance comparator: python3 is required" >&2
  exit 2
fi

exec python3 "${SCRIPT_DIR}/compare_order_book_acceptance.py" "$@"
