#!/usr/bin/env python3

import re
import platform
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


SCHEDULE_ID = "fixed_block_offset_v1_splitmix64_seed_61737472612d6974"
HOT_ARENA_SCHEMA = "redesign_v1"
ARENA_ALIGNMENT = 2 * 1024 * 1024
ARENA_IDS = (
    "order_direct",
    "order_fallback",
    "book_descriptors",
    "price_roots",
    "price_prepared_books",
    "price_pages",
    "price_page_owners",
    "price_page_summaries",
    "price_page_occupancy",
    "price_book_summaries",
    "price_book_occupancy",
)
SEMANTIC_SCHEMA = "applied_itch_book_semantics_v1_fnv1a64le"
EXPECTED_SEMANTIC_DIGEST = "12141299839370961608"
CAPACITY_IDENTITY_FIELDS = (
    "capacity_profile_bound",
    "capacity_evidence_schema",
    "capacity_profile_name",
    "capacity_evidence_sha256",
    "capacity_corpus_manifest_sha256",
    "capacity_profiler_sha256",
    "capacity_profiled_max_order_ref",
    "capacity_profiled_unique_price_pages",
    "capacity_minimum_direct_order_headroom",
    "capacity_effective_direct_order_headroom",
    "capacity_minimum_price_page_headroom",
    "capacity_effective_price_page_headroom",
)


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


def make_trace() -> bytes:
    messages = [
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
    ]
    return b"".join(struct.pack(">H", len(message)) + message
                    for message in messages)


def invoke(binary: str, arguments, expect_success: bool = True):
    result = subprocess.run(
        [binary, *arguments], capture_output=True, text=True, check=False
    )
    if (result.returncode == 0) != expect_success:
        raise AssertionError(
            f"unexpected exit {result.returncode}\nstdout:\n{result.stdout}"
            f"\nstderr:\n{result.stderr}"
        )
    return result


def record(output: str, prefix: str) -> str:
    matches = [line for line in output.splitlines()
               if line.startswith(prefix + " ")]
    if len(matches) != 1:
        raise AssertionError(f"expected one {prefix} record, got {matches}")
    return matches[0]


def fields(line: str):
    return dict(re.findall(r"(?:^| )([A-Za-z0-9_]+)=(\"[^\"]*\"|\S+)", line))


def validate_schedule(output: str, sample_every: str = "1",
                      warmup: str = "0"):
    storage = fields(record(output, "itch_book_replay_storage_plan"))
    ready = fields(record(output, "itch_book_replay_ready"))
    final = fields(record(output, "itch_book_replay"))
    assert storage["hot_arena_schema"] == HOT_ARENA_SCHEMA
    assert ready["hot_arena_schema"] == HOT_ARENA_SCHEMA
    assert final["hot_arena_schema"] == HOT_ARENA_SCHEMA
    assert ready["sample_schedule_id"] == SCHEDULE_ID
    assert final["sample_schedule_id"] == SCHEDULE_ID
    assert ready["sample_every"] == final["sample_every"] == sample_every
    assert ready["warmup_book_messages"] == final["warmup_book_messages"] == warmup
    for key in CAPACITY_IDENTITY_FIELDS:
        assert storage[key] == ready[key] == final[key]
    assert storage["direct_order_slots"] == final["direct_order_slots"]
    assert storage["fallback_buckets"] == final["fallback_buckets"]
    assert storage["price_page_capacity"] == final["price_page_capacity"]
    expected_prelude_bytes = sum(
        2 + len(message) for message in (directory(), event("S"))
    )
    assert ready["prelude_records"] == final["prelude_records"] == "2"
    assert ready["prelude_bytes"] == final["prelude_bytes"] == str(
        expected_prelude_bytes
    )

    ranges = []
    mapped_sizes = {}
    for arena_id in ARENA_IDS:
        mapped_bytes = int(storage[f"{arena_id}_mapped_bytes"])
        ready_base = int(ready[f"{arena_id}_base"])
        ready_mapped_bytes = int(ready[f"{arena_id}_mapped_bytes"])
        effective_mapped_bytes = int(
            final[f"effective_{arena_id}_mapped_bytes"]
        )
        assert mapped_bytes > 0
        assert mapped_bytes % ARENA_ALIGNMENT == 0
        assert ready_base > 0
        assert ready_base % ARENA_ALIGNMENT == 0
        assert ready_mapped_bytes == mapped_bytes
        assert effective_mapped_bytes == mapped_bytes
        mapped_sizes[arena_id] = mapped_bytes
        ranges.append((ready_base, ready_base + ready_mapped_bytes, arena_id))

    # The schema declares exactly these arena address fields. The historical
    # direct_orders_base spelling remains only as a checked compatibility alias.
    base_fields = {key.removesuffix("_base") for key in ready
                   if key.endswith("_base")}
    assert base_fields == set(ARENA_IDS) | {"direct_orders"}
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        assert previous[1] <= current[0], (previous, current)

    core_sum = sum(size for arena_id, size in mapped_sizes.items()
                   if arena_id != "book_descriptors")
    all_arena_sum = sum(mapped_sizes.values())
    assert int(storage["mapped_array_bytes"]) == core_sum
    assert int(storage["planned_storage_bytes"]) == all_arena_sum
    assert 0 < int(storage["descriptor_bytes"]) <= \
        mapped_sizes["book_descriptors"]
    assert int(storage["direct_orders_mapped_bytes"]) == \
        mapped_sizes["order_direct"]
    assert int(ready["direct_orders_base"]) == int(ready["order_direct_base"])
    assert int(ready["direct_orders_mapped_bytes"]) == \
        mapped_sizes["order_direct"]
    assert int(final["effective_mapped_bytes"]) == core_sum
    assert int(final["effective_direct_orders_mapped_bytes"]) == \
        mapped_sizes["order_direct"]
    assert int(final["effective_descriptor_bytes"]) == \
        int(storage["descriptor_bytes"])
    assert int(final["effective_storage_bytes"]) == all_arena_sum
    return final


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: ItchBookReplayIdentityTest.py <benchmark>")
    binary = sys.argv[1]
    trace = make_trace()
    with tempfile.TemporaryDirectory(prefix="astra-replay-identity-") as temp:
        trace_path = Path(temp) / "all-mutations.itch"
        trace_path.write_bytes(trace)
        capacity_manifest = (
            "schema=astra_book_capacity_evidence_v1\n"
            "profile_name=replay-identity-test-v1\n"
            f"corpus_manifest_sha256={'a' * 64}\n"
            f"profiler_sha256={'b' * 64}\n"
            "order_direct_slots=8\n"
            "order_fallback_buckets=2\n"
            "price_page_capacity=3\n"
            "profiled_max_order_ref=5\n"
            "profiled_unique_price_pages=1\n"
            "minimum_direct_order_headroom=2\n"
            "minimum_price_page_headroom=2\n"
        ).encode("ascii")
        capacity_path = Path(temp) / "capacity-evidence.txt"
        capacity_path.write_bytes(capacity_manifest)
        capacity_sha256 = hashlib.sha256(capacity_manifest).hexdigest()
        bound_plan = invoke(binary, [
            str(trace_path),
            "--storage-plan-only",
            "--capacity-profile-name=replay-identity-test-v1",
            f"--capacity-evidence-file={capacity_path}",
            f"--capacity-evidence-sha256={capacity_sha256}",
            "--direct-order-slots=8",
            "--fallback-buckets=2",
            "--price-page-capacity=3",
        ])
        bound_fields = fields(
            record(bound_plan.stdout, "itch_book_replay_storage_plan")
        )
        assert bound_fields["capacity_profile_bound"] == "1"
        assert bound_fields["capacity_evidence_schema"] == \
            "astra_book_capacity_evidence_v1"
        assert bound_fields["capacity_profile_name"] == \
            "replay-identity-test-v1"
        assert bound_fields["capacity_evidence_sha256"] == capacity_sha256
        assert bound_fields["capacity_effective_direct_order_headroom"] == "2"
        assert bound_fields["capacity_effective_price_page_headroom"] == "2"
        mismatched_plan = invoke(binary, [
            str(trace_path),
            "--storage-plan-only",
            "--capacity-profile-name=replay-identity-test-v1",
            f"--capacity-evidence-file={capacity_path}",
            f"--capacity-evidence-sha256={capacity_sha256}",
            "--direct-order-slots=9",
        ], expect_success=False)
        assert "differs from capacity evidence manifest" in mismatched_plan.stderr

        common_args = [
            str(trace_path),
            "--sample-every=1",
            "--warmup-book-messages=0",
            "--min-samples=8",
            "--price-page-capacity=3",
            "--sample-capacity=16",
            "--expect-records=11",
            f"--expect-bytes={len(trace)}",
        ]

        bound_replay = invoke(binary, [
            *common_args,
            "--capacity-profile-name=replay-identity-test-v1",
            f"--capacity-evidence-file={capacity_path}",
            f"--capacity-evidence-sha256={capacity_sha256}",
            "--direct-order-slots=8",
            "--fallback-buckets=2",
        ])
        bound_final = validate_schedule(bound_replay.stdout)
        assert bound_final["capacity_profile_bound"] == "1"
        assert bound_final["capacity_evidence_sha256"] == capacity_sha256
        assert bound_final["capacity_profiled_max_order_ref"] == "5"
        assert bound_final["capacity_profiled_unique_price_pages"] == "1"

        direct = invoke(binary, [
            *common_args,
            "--direct-order-slots=8",
            "--fallback-buckets=2",
            "--mutation-digest",
        ])
        direct_fields = validate_schedule(direct.stdout)
        assert direct_fields["mutation_digest_enabled"] == "1"
        assert direct_fields["semantic_mutation_digest_enabled"] == "1"
        assert direct_fields["semantic_mutation_digest_schema"] == SEMANTIC_SCHEMA
        physical_digest = direct_fields["mutation_digest"]
        semantic_digest = direct_fields["semantic_mutation_digest"]
        assert semantic_digest == EXPECTED_SEMANTIC_DIGEST

        verified = invoke(binary, [
            *common_args,
            "--direct-order-slots=8",
            "--fallback-buckets=2",
            f"--expect-mutation-digest={physical_digest}",
            f"--expect-semantic-mutation-digest={semantic_digest}",
        ])
        assert validate_schedule(verified.stdout)[
            "semantic_mutation_digest"
        ] == semantic_digest

        fallback = invoke(binary, [
            *common_args,
            "--direct-order-slots=1",
            "--fallback-buckets=8",
            f"--expect-semantic-mutation-digest={semantic_digest}",
        ])
        fallback_fields = validate_schedule(fallback.stdout)
        assert fallback_fields["semantic_mutation_digest"] == semantic_digest
        assert fallback_fields["mutation_digest"] != physical_digest

        latency = invoke(binary, [
            *common_args,
            "--direct-order-slots=8",
            "--fallback-buckets=2",
        ])
        latency_fields = validate_schedule(latency.stdout)
        assert latency_fields["mutation_digest_enabled"] == "0"
        assert latency_fields["semantic_mutation_digest_enabled"] == "0"
        assert "mutation_digest" not in latency_fields
        assert "semantic_mutation_digest" not in latency_fields
        assert "semantic_mutation_digest_schema" not in latency_fields

        schedule_probe_args = [
            str(trace_path),
            "--sample-every=3",
            "--warmup-book-messages=0",
            "--min-samples=3",
            "--price-page-capacity=3",
            "--sample-capacity=16",
            "--expect-records=11",
            f"--expect-bytes={len(trace)}",
            "--direct-order-slots=8",
            "--fallback-buckets=2",
        ]
        schedule_probe = invoke(binary, schedule_probe_args)
        schedule_fields = validate_schedule(schedule_probe.stdout, "3", "0")
        assert schedule_fields["sample_count"] == "3"
        type_counts = {}
        for line in schedule_probe.stdout.splitlines():
            if line.startswith("itch_book_replay_type "):
                type_fields = fields(line)
                type_counts[type_fields["type"]] = type_fields["sample_count"]
        assert type_counts == {
            "A": "0", "F": "0", "E": "1", "C": "1",
            "X": "0", "D": "1", "U": "0",
        }

        machine = platform.machine().lower()
        x86 = machine in ("x86_64", "amd64", "i386", "i486", "i586", "i686")
        clock_gate = invoke(binary, [
            *common_args,
            "--direct-order-slots=8",
            "--fallback-buckets=2",
            "--max-p50-ns=18446744073709551615",
        ], expect_success=x86)
        if not x86:
            assert "RDTSCP is unavailable" in clock_gate.stderr

        rejected = invoke(binary, [
            *common_args,
            "--direct-order-slots=8",
            "--fallback-buckets=2",
            f"--expect-semantic-mutation-digest={semantic_digest}",
            "--max-p50-ns=1",
        ], expect_success=False)
        assert "require separate runs" in rejected.stderr

    return 0


if __name__ == "__main__":
    sys.exit(main())
