#!/usr/bin/env python3
"""Layout-independent branch-5 replay identity and lifecycle proof."""

import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


SCHEDULE_ID = "fixed_block_offset_v1_splitmix64_seed_61737472612d6974"
SEMANTIC_SCHEMA = "applied_itch_book_semantics_v1_fnv1a64le"
EXPECTED_IDENTITY_DIGEST = "12141299839370961608"


def common(message_type: str, locate: int = 1) -> bytes:
    return (
        message_type.encode("ascii")
        + struct.pack(">HH", locate, 7)
        + (1).to_bytes(6, "big")
    )


def stock(value: str) -> bytes:
    return value.encode("ascii").ljust(8, b" ")


def directory() -> bytes:
    message = (
        common("R")
        + stock("SEMANTIC")
        + b"QN"
        + struct.pack(">I", 100)
        + b"NC  P"
    )
    return message.ljust(39, b" ")


def event(code: str) -> bytes:
    return common("S", 0) + code.encode("ascii")


def add(message_type: str, reference: int, side: str, quantity: int,
        price: int) -> bytes:
    message = (
        common(message_type)
        + struct.pack(">Q", reference)
        + side.encode("ascii")
        + struct.pack(">I", quantity)
        + stock("SEMANTIC")
        + struct.pack(">I", price)
    )
    return message + (b"MPID" if message_type == "F" else b"")


def execute(message_type: str, reference: int, quantity: int) -> bytes:
    message = (
        common(message_type)
        + struct.pack(">QI", reference, quantity)
        + struct.pack(">Q", 9001)
    )
    if message_type == "C":
        message += b"Y" + struct.pack(">I", 123456)
    return message


def cancel(reference: int, quantity: int) -> bytes:
    return common("X") + struct.pack(">QI", reference, quantity)


def replace(old_reference: int, new_reference: int, quantity: int,
            price: int) -> bytes:
    return common("U") + struct.pack(
        ">QQII", old_reference, new_reference, quantity, price
    )


def delete(reference: int) -> bytes:
    return common("D") + struct.pack(">Q", reference)


def framed(messages) -> bytes:
    return b"".join(struct.pack(">H", len(message)) + message
                    for message in messages)


def identity_trace() -> bytes:
    return framed([
        directory(),
        event("S"),
        add("A", 1, "B", 100, 0xFFFFFFFF),
        add("F", 2, "S", 80, 0),
        execute("E", 1, 10),
        execute("C", 2, 20),
        cancel(1, 5),
        replace(1, 3, 70, 0x7FFF0001),
        delete(2),
        delete(3),
        event("C"),
    ])


def lifecycle_trace() -> bytes:
    return framed([
        directory(),
        event("S"),
        directory(),  # repeated R after the sealed-universe boundary
        add("A", 41, "B", 100, 123_400),
        event("E"),  # End of System Hours
        execute("E", 41, 40),  # independent Order Executed afterwards
        delete(41),
        event("C"),
    ])


def fields(line: str):
    return dict(re.findall(
        r'(?:^| )([A-Za-z0-9_]+)=("[^"]*"|\S+)', line
    ))


def one_record(output: str, prefix: str):
    matches = [line for line in output.splitlines()
               if line.startswith(prefix + " ")]
    if len(matches) != 1:
        raise AssertionError(f"expected one {prefix} line, got {matches}")
    return fields(matches[0])


def invoke(binary: str, trace_path: Path, trace: bytes, records: int,
           samples: int, manifest_path: Path, backend_arguments):
    arguments = [
        str(trace_path),
        "--sample-every=1",
        "--warmup-book-messages=0",
        f"--min-samples={samples}",
        f"--sample-capacity={max(16, samples)}",
        f"--expect-records={records}",
        f"--expect-bytes={len(trace)}",
        "--mutation-digest",
    ]
    arguments.extend(argument.replace("{manifest}", str(manifest_path))
                     for argument in backend_arguments)
    result = subprocess.run(
        [binary, *arguments], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise AssertionError(
            f"replay exited {result.returncode}\nstdout:\n{result.stdout}"
            f"\nstderr:\n{result.stderr}"
        )
    return result.stdout


def require_common(final, records: int, bytes_count: int, messages: int):
    expected = {
        "records": str(records),
        "bytes": str(bytes_count),
        "book_messages": str(messages),
        "applied_book_mutations": str(messages),
        "sample_count": str(messages),
        "sample_schedule_id": SCHEDULE_ID,
        "sample_every": "1",
        "warmup_book_messages": "0",
        "final_live_orders": "0",
        "phase": "7",
        "mutation_digest_enabled": "1",
        "semantic_mutation_digest_enabled": "1",
        "semantic_mutation_digest_schema": SEMANTIC_SCHEMA,
    }
    for key, value in expected.items():
        if final.get(key) != value:
            raise AssertionError(f"{key}: {final.get(key)!r} != {value!r}")


def require_post_s_boundary(output: str):
    ready = one_record(output, "itch_book_replay_ready")
    final = one_record(output, "itch_book_replay")
    expected = {
        "prelude_records": "2",
        "prelude_bytes": str(len(framed([directory(), event("S")]))),
    }
    for key, value in expected.items():
        if ready.get(key) != value or final.get(key) != value:
            raise AssertionError(
                f"{key}: ready={ready.get(key)!r} final={final.get(key)!r} "
                f"expected={value!r}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--expect-hot-arena-schema")
    arguments = sys.argv[1:]
    if "--" in arguments:
        separator = arguments.index("--")
        parser_arguments = arguments[:separator]
        backend_arguments = arguments[separator + 1:]
    else:
        parser_arguments = arguments
        backend_arguments = []
    options = parser.parse_args(parser_arguments)

    with tempfile.TemporaryDirectory(prefix="astra-branch5-identity-") as temp:
        temp_path = Path(temp)

        identity = identity_trace()
        identity_path = temp_path / "all-mutations.itch"
        identity_path.write_bytes(identity)
        identity_output = invoke(
            options.binary,
            identity_path,
            identity,
            11,
            8,
            temp_path / "identity-native-ranges.txt",
            backend_arguments,
        )
        identity_final = one_record(identity_output, "itch_book_replay")
        require_common(identity_final, 11, len(identity), 8)
        require_post_s_boundary(identity_output)
        if identity_final.get("semantic_mutation_digest") != \
                EXPECTED_IDENTITY_DIGEST:
            raise AssertionError(
                "layout-independent identity digest mismatch: "
                f"{identity_final.get('semantic_mutation_digest')}"
            )

        lifecycle = lifecycle_trace()
        lifecycle_path = temp_path / "late-r-post-hours-e.itch"
        lifecycle_path.write_bytes(lifecycle)
        lifecycle_output = invoke(
            options.binary,
            lifecycle_path,
            lifecycle,
            8,
            3,
            temp_path / "lifecycle-native-ranges.txt",
            backend_arguments,
        )
        lifecycle_final = one_record(lifecycle_output, "itch_book_replay")
        require_common(lifecycle_final, 8, len(lifecycle), 3)
        require_post_s_boundary(lifecycle_output)

        if options.expect_hot_arena_schema:
            for output in (identity_output, lifecycle_output):
                for prefix in (
                    "itch_book_replay_storage_plan",
                    "itch_book_replay_ready",
                    "itch_book_replay",
                ):
                    actual = one_record(output, prefix).get("hot_arena_schema")
                    if actual != options.expect_hot_arena_schema:
                        raise AssertionError(
                            f"{prefix} schema {actual!r} != "
                            f"{options.expect_hot_arena_schema!r}"
                        )

    print(
        "branch5_identity result=PASS records=11 applied=8 "
        f"semantic_digest={EXPECTED_IDENTITY_DIGEST} "
        "late_r_after_ss=PASS order_e_after_system_e=PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
