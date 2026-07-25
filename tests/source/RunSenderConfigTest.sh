#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/run_sender.sh" >&2
  exit 2
fi

script=$1

help_output=$(bash "$script" --help)
[[ $help_output == *"ASTRA_SENDER_SKIP_BUILD=on"* ]]

missing_trace="/tmp/astra-run-sender-config-test-missing-$$"
if output=$(ASTRA_SENDER_SKIP_BUILD=on \
    bash "$script" "$missing_trace" 2>&1); then
  echo "expected missing sender trace to fail" >&2
  echo "$output" >&2
  exit 1
fi
[[ $output == *"Using prebuilt synchronized ITCH A/B feeder:"* ]]
[[ $output == *"ITCH file not found:"* ]]
if [[ $output == *"Configuring synchronized ITCH A/B feeder"* ]]; then
  echo "skip-build mode unexpectedly configured the project" >&2
  exit 1
fi

if output=$(ASTRA_SENDER_SKIP_BUILD=invalid \
    bash "$script" "$missing_trace" 2>&1); then
  echo "expected invalid skip-build value to fail" >&2
  echo "$output" >&2
  exit 1
fi
[[ $output == *"Unknown ASTRA_SENDER_SKIP_BUILD value: invalid"* ]]
if [[ $output == *"Configuring synchronized ITCH A/B feeder"* ]]; then
  echo "invalid skip-build value reached configuration" >&2
  exit 1
fi
