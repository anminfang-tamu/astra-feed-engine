#!/usr/bin/env python3

import platform
import re
import subprocess
import sys


WORKLOADS = (
    "direct_partial_execute",
    "direct_partial_execute_wide_pages",
    "direct_partial_cancel_wide_pages",
    "add_existing_level",
    "delete_populated_level",
    "replace_cross_page",
    "remove_current_best",
    "remove_cross_page_best",
    "fallback_partial_execute",
    "fallback_four_slot_miss",
)


def invoke(binary: str, arguments, expect_success: bool):
    result = subprocess.run(
        [binary, *arguments], capture_output=True, text=True, check=False
    )
    if (result.returncode == 0) != expect_success:
        raise AssertionError(
            f"unexpected exit {result.returncode} for {arguments!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def assert_rejected(binary: str, arguments, diagnostic: str):
    result = invoke(binary, arguments, expect_success=False)
    if diagnostic not in result.stderr:
        raise AssertionError(
            f"missing diagnostic {diagnostic!r} for {arguments!r}\n"
            f"stderr:\n{result.stderr}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: OrderBookBenchmarkCliTest.py <benchmark>")
    binary = sys.argv[1]

    help_result = invoke(binary, ["--help"], expect_success=True)
    for workload in WORKLOADS:
        assert workload in help_result.stdout

    smoke = invoke(binary, ["--iterations=1"], expect_success=True)
    records = {}
    for line in smoke.stdout.splitlines():
        match = re.fullmatch(
            r"workload=(\S+) iterations=(\d+) p50_ns=(\d+) p90_ns=(\d+) "
            r"p99_ns=(\d+) p99_9_ns=(\d+) max_ns=(\d+)",
            line,
        )
        if match:
            name = match.group(1)
            if name in records:
                raise AssertionError(f"duplicate workload record: {name}")
            values = tuple(int(value) for value in match.groups()[2:])
            if tuple(sorted(values)) != values:
                raise AssertionError(f"non-monotonic distribution: {line}")
            records[name] = (int(match.group(2)), values)
    assert tuple(records) == WORKLOADS
    assert all(iterations == 1 for iterations, _ in records.values())
    assert "fallback_setup " in smoke.stdout
    assert "checksum=" in smoke.stdout

    rejected_cases = (
        (["--iterations=0"], "iterations must be in"),
        (["--iterations=1000001"], "iterations must be in"),
        (["--iterations=abc"], "invalid iterations"),
        (["--unknown"], "unknown benchmark option"),
        (["--gate=unknown:1:2:3"], "unknown gate workload"),
        (["--gate=direct_partial_execute:1:2"], "must have"),
        (["--gate=direct_partial_execute:1:2:3:4"], "must have"),
        (["--gate=direct_partial_execute:0:2:3"], "must be positive"),
        (["--gate=direct_partial_execute:2:1:3"], "must be monotonic"),
        (["--gate=direct_partial_execute:1:3:2"], "must be monotonic"),
        (
            [
                "--gate=delete_populated_level:1:2:3",
                "--gate=delete_populated_level:4:5:6",
            ],
            "duplicate gate for workload",
        ),
        (
            [
                "--gate=direct_partial_execute:1:2:3",
                "--max-direct-p50-ns=0",
            ],
            "duplicate gate for workload",
        ),
        (
            ["--max-direct-p50-ns=2", "--max-direct-p99-ns=1"],
            "must be monotonic",
        ),
        (
            ["--max-direct-p50-ns=1", "--max-direct-p50-ns=2"],
            "duplicate direct p50 gate",
        ),
    )
    for arguments, diagnostic in rejected_cases:
        assert_rejected(binary, arguments, diagnostic)

    # Explicit legacy zeroes disable their gates and remain portable.
    invoke(
        binary,
        [
            "--iterations=1",
            "--max-direct-p50-ns=0",
            "--max-direct-p99-ns=0",
            "--max-direct-p99-9-ns=0",
        ],
        expect_success=True,
    )

    machine = platform.machine().lower()
    x86 = machine in ("x86_64", "amd64", "i386", "i486", "i586", "i686")
    gate = invoke(
        binary,
        [
            "--iterations=1",
            "--gate=delete_populated_level:18446744073709551615:"
            "18446744073709551615:18446744073709551615",
        ],
        expect_success=x86,
    )
    if not x86:
        assert "require x86 RDTSCP" in gate.stderr
        assert "workload=" not in gate.stdout

    return 0


if __name__ == "__main__":
    sys.exit(main())
