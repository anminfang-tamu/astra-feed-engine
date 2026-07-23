#!/usr/bin/env python3
"""Isolated fixtures for compare_order_book_acceptance.sh."""

from __future__ import annotations

import hashlib
import re
import shlex
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Mapping, Union


ROOT = Path(__file__).resolve().parents[2]
COMPARATOR = ROOT / "scripts/compare_order_book_acceptance.sh"
BASELINE_BRANCH = "5-to-add-numa"
CANDIDATE_BRANCH = "6-redesign-order-book-data-structure"
BASELINE_COMMIT = "1" * 40
CANDIDATE_COMMIT = "2" * 40
TRACE_SHA256 = "a" * 64
EXPECTED_RECORDS = 123
EXPECTED_BYTES = 456
PRELUDE_RECORDS = 17
PRELUDE_BYTES = 91
SCHEDULE_ID = "fixed_block_offset_v1_splitmix64_seed_61737472612d6974"
SEMANTIC_SCHEMA = "applied_itch_book_semantics_v1_fnv1a64le"
REDESIGN_SCHEMA = "redesign_v1"
BRANCH5_SCHEMA = "branch5_native_v1"
NATIVE_MANIFEST_SCHEMA = "branch5_native_ranges_v1"
BRANCH5_PINNED_COMMIT = "324d81a15ee52cc72f68873a1ced122923406df2"
BRANCH5_COMPATIBILITY_TREE = "492938730e6db91e84bdb1f8e25152536e81dbc0"
BRANCH5_PINNED_TRACE_SHA256 = (
    "1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3"
)
BRANCH5_PINNED_CAPACITY_PROFILE = "nasdaq-itch-20190130-branch5-native-v1"
BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256 = (
    "05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282"
)
BRANCH5_PRICE_VECTOR_BYTES = {
    "price_nodes_bytes": 178257920,
    "price_leaves_bytes": 2214592512,
    "price_levels_bytes": 50331648,
    "price_free_nodes_bytes": 655360,
    "price_free_leaves_bytes": 4194304,
    "price_free_levels_bytes": 8388608,
}
BRANCH5_PRICE_POOL_BYTES = 2456420352
BRANCH5_PLAN_CAPACITIES = {
    "default_order_capacity": 65536,
    "price_internal_node_capacity": 163840,
    "price_leaf_capacity": 1048576,
    "price_level_capacity": 2097152,
}
BRANCH5_PLANNED_BYTES = 68719476736
BRANCH5_RESERVE_BYTES = 17179869184
MESSAGE_TYPES = ("A", "F", "E", "C", "X", "D", "U")
TYPE_SAMPLES = {"A": 10, "F": 10, "E": 10, "C": 10, "X": 10, "D": 10, "U": 40}
SUMMARY_HEADER = (
    "kind\tid\tstatus\texit_code\tp50_ns\tp99_ns\tp99_9_ns\tdetail\tstdout\tstderr\n"
)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


CAPACITY_PROFILE_NAME = "fixture-capacity-v1"
CAPACITY_CORPUS_SHA256 = "c" * 64
CAPACITY_PROFILER_SHA256 = "d" * 64
CAPACITY_DIRECT_SLOTS = 128
CAPACITY_FALLBACK_BUCKETS = 8
CAPACITY_PRICE_PAGES = 100
CAPACITY_PROFILED_MAX_ORDER_REF = 99
CAPACITY_PROFILED_UNIQUE_PRICE_PAGES = 50
CAPACITY_MINIMUM_DIRECT_HEADROOM = 20
CAPACITY_EFFECTIVE_DIRECT_HEADROOM = (
    CAPACITY_DIRECT_SLOTS - (CAPACITY_PROFILED_MAX_ORDER_REF + 1)
)
CAPACITY_MINIMUM_PRICE_PAGE_HEADROOM = 25
CAPACITY_EFFECTIVE_PRICE_PAGE_HEADROOM = (
    CAPACITY_PRICE_PAGES - CAPACITY_PROFILED_UNIQUE_PRICE_PAGES
)
CAPACITY_MANIFEST_BYTES = (
    "schema=astra_book_capacity_evidence_v1\n"
    f"profile_name={CAPACITY_PROFILE_NAME}\n"
    f"corpus_manifest_sha256={CAPACITY_CORPUS_SHA256}\n"
    f"profiler_sha256={CAPACITY_PROFILER_SHA256}\n"
    f"order_direct_slots={CAPACITY_DIRECT_SLOTS}\n"
    f"order_fallback_buckets={CAPACITY_FALLBACK_BUCKETS}\n"
    f"price_page_capacity={CAPACITY_PRICE_PAGES}\n"
    f"profiled_max_order_ref={CAPACITY_PROFILED_MAX_ORDER_REF}\n"
    f"profiled_unique_price_pages={CAPACITY_PROFILED_UNIQUE_PRICE_PAGES}\n"
    f"minimum_direct_order_headroom={CAPACITY_MINIMUM_DIRECT_HEADROOM}\n"
    f"minimum_price_page_headroom={CAPACITY_MINIMUM_PRICE_PAGE_HEADROOM}\n"
).encode("ascii")
CAPACITY_EVIDENCE_SHA256 = digest(CAPACITY_MANIFEST_BYTES)
CAPACITY_REPLAY_IDENTITY: Dict[str, Union[str, int]] = {
    "capacity_profile_bound": 1,
    "capacity_evidence_schema": "astra_book_capacity_evidence_v1",
    "capacity_profile_name": CAPACITY_PROFILE_NAME,
    "capacity_evidence_sha256": CAPACITY_EVIDENCE_SHA256,
    "capacity_corpus_manifest_sha256": CAPACITY_CORPUS_SHA256,
    "capacity_profiler_sha256": CAPACITY_PROFILER_SHA256,
    "capacity_profiled_max_order_ref": CAPACITY_PROFILED_MAX_ORDER_REF,
    "capacity_profiled_unique_price_pages": CAPACITY_PROFILED_UNIQUE_PRICE_PAGES,
    "capacity_minimum_direct_order_headroom": CAPACITY_MINIMUM_DIRECT_HEADROOM,
    "capacity_effective_direct_order_headroom": CAPACITY_EFFECTIVE_DIRECT_HEADROOM,
    "capacity_minimum_price_page_headroom": CAPACITY_MINIMUM_PRICE_PAGE_HEADROOM,
    "capacity_effective_price_page_headroom": CAPACITY_EFFECTIVE_PRICE_PAGE_HEADROOM,
}
CAPACITY_REPLAY_FIELD_NAMES = tuple(CAPACITY_REPLAY_IDENTITY)


def write(root: Path, relative: str, data: Union[str, bytes] = "") -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8")
    else:
        path.write_bytes(data)


def equals_text(fields: Mapping[str, Union[str, int]]) -> str:
    return "".join(f"{key}={value}\n" for key, value in fields.items())


def argv_bytes(arguments: List[str]) -> bytes:
    return b"\0".join(argument.encode("utf-8") for argument in arguments) + b"\0"


def metrics(role: str, workload: str, run: int, tail_regression: bool = False) -> Dict[str, int]:
    index = 0 if workload == "aggregate" else MESSAGE_TYPES.index(workload) + 1
    if role == "baseline":
        p50 = 280 + run + index
        p90 = 330 + run + index
        p99 = 400 + run + index
        p999 = 500 + run + index
        maximum = 800 + index
    else:
        p50 = 100 + run + index
        p90 = 180 + run + index
        p99 = 260 + run + index
        p999 = 350 + run + index
        maximum = 700 + index
        if tail_regression and workload == "A" and run == 3:
            p99 = 1000
            p999 = 1100
            maximum = 1200
    return {
        "p50_ns": p50,
        "p90_ns": p90,
        "p99_ns": p99,
        "p99_9_ns": p999,
        "max_ns": maximum,
    }


def native_ranges():
    ranges = (
        ("price_nodes", 0, BRANCH5_PRICE_VECTOR_BYTES["price_nodes_bytes"]),
        ("price_leaves", 0, BRANCH5_PRICE_VECTOR_BYTES["price_leaves_bytes"]),
        ("price_levels", 0, BRANCH5_PRICE_VECTOR_BYTES["price_levels_bytes"]),
        (
            "price_free_nodes",
            0,
            BRANCH5_PRICE_VECTOR_BYTES["price_free_nodes_bytes"],
        ),
        (
            "price_free_leaves",
            0,
            BRANCH5_PRICE_VECTOR_BYTES["price_free_leaves_bytes"],
        ),
        (
            "price_free_levels",
            0,
            BRANCH5_PRICE_VECTOR_BYTES["price_free_levels_bytes"],
        ),
        ("order_records", 1, 32 * 65536),
        ("order_free_indices", 1, 4 * 65536),
        ("order_occupancy", 1, 65536 // 8),
        ("order_ref_entries", 1, 64 * 65536),
    )
    rows = []
    base = 100_000
    for kind, locate, size in ranges:
        rows.append((kind, locate, base, size))
        base += size + 4096
    digest_value = 14695981039346656037
    for byte in NATIVE_MANIFEST_SCHEMA.encode("ascii"):
        digest_value = ((digest_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    for kind, locate, base, size in rows:
        encoded = kind.encode("ascii")
        payload = struct.pack("<Q", len(encoded)) + encoded + struct.pack(
            "<QQQ", locate, base, size
        )
        for byte in payload:
            digest_value = ((digest_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return rows, sum(row[3] for row in rows), digest_value


NATIVE_ROWS, NATIVE_BYTES, NATIVE_DIGEST = native_ranges()


def native_manifest_text() -> str:
    lines = [
        f"branch5_native_ranges schema={NATIVE_MANIFEST_SCHEMA} "
        f"count={len(NATIVE_ROWS)} bytes={NATIVE_BYTES} digest={NATIVE_DIGEST}"
    ]
    for ordinal, (kind, locate, base, size) in enumerate(NATIVE_ROWS):
        lines.append(
            f"branch5_native_range ordinal={ordinal} kind={kind} "
            f"locate={locate} base={base} bytes={size}"
        )
    return "\n".join(lines) + "\n"


def replay_output(
    role: str,
    run: int,
    correctness: bool,
    p50_failure: bool = False,
    tail_regression: bool = False,
) -> str:
    per_type = {
        message_type: metrics(role, message_type, run, tail_regression)
        for message_type in MESSAGE_TYPES
    }
    aggregate = metrics(role, "aggregate", run, tail_regression)
    aggregate["max_ns"] = max(values["max_ns"] for values in per_type.values())
    if p50_failure and role == "candidate" and run == 2:
        aggregate["p50_ns"] = 151
        aggregate["p90_ns"] = max(aggregate["p90_ns"], 151)

    schema = BRANCH5_SCHEMA if role == "baseline" else REDESIGN_SCHEMA
    ready: Dict[str, Union[str, int]] = {
        "hot_arena_schema": schema,
        "prefault": 1,
        "prelude_records": PRELUDE_RECORDS,
        "prelude_bytes": PRELUDE_BYTES,
        "sample_capacity": 1000,
        "sample_storage_prefaulted": 1,
        "start_gate_enabled": 1,
        "sample_every": 64,
        "warmup_book_messages": 1000000,
        "sample_schedule_id": SCHEDULE_ID,
    }
    if role == "baseline":
        ready.update(
            {
                "native_prefault_complete": 1,
                "book_universe_sealed": 1,
                "prepared_books": 1,
                "native_range_manifest_schema": NATIVE_MANIFEST_SCHEMA,
                "native_range_manifest": '"fixture-native-ranges.txt"',
                "native_range_count": len(NATIVE_ROWS),
                "native_range_bytes": NATIVE_BYTES,
                "native_range_digest": NATIVE_DIGEST,
            }
        )
    else:
        ready.update(
            {
                "effective_storage_bytes": 3000,
                "direct_orders_base": 100000,
                "direct_orders_mapped_bytes": 1000,
                "price_pages_base": 200000,
                "price_pages_mapped_bytes": 1000,
                **CAPACITY_REPLAY_IDENTITY,
            }
        )
    main: Dict[str, Union[str, int]] = {
        "hot_arena_schema": schema,
        "records": EXPECTED_RECORDS,
        "bytes": EXPECTED_BYTES,
        "prelude_records": PRELUDE_RECORDS,
        "prelude_bytes": PRELUDE_BYTES,
        "book_messages": 1000,
        "applied_book_mutations": 1000,
        "sample_count": 100,
        "sample_every": 64,
        "warmup_book_messages": 1000000,
        "min_samples": 100,
        "sample_capacity": 1000,
        "sample_storage_prefaulted": 1,
        "post_warmup_minor_faults": 0,
        "post_warmup_major_faults": 0,
        "prefault": 1,
        "storage_system_page_bytes": 4096,
        "rdtsc_overhead_ticks": 20,
        "rdtsc_ticks_per_second": 3000000000,
        "now_ns_overhead_ns": 10,
        "price_capacity_failures": 0,
        "final_live_orders": 0,
        "phase": 7,
        "mutation_digest_enabled": 1 if correctness else 0,
        "semantic_mutation_digest_enabled": 1 if correctness else 0,
        "sample_strategy": "fixed_seed_block_offset",
        "sample_schedule_id": SCHEDULE_ID,
        **aggregate,
    }
    if role == "baseline":
        main.update(
            {
                "native_prefault_complete": 1,
                "book_universe_sealed": 1,
                "prepared_books": 1,
                "native_range_manifest_schema": NATIVE_MANIFEST_SCHEMA,
                "native_range_manifest": '"fixture-native-ranges.txt"',
                "native_range_count": len(NATIVE_ROWS),
                "native_range_bytes": NATIVE_BYTES,
                "native_range_digest": NATIVE_DIGEST,
                "price_internal_node_exhaustions": 0,
                "price_leaf_exhaustions": 0,
                "price_level_exhaustions": 0,
            }
        )
    else:
        main.update(
            {
                "direct_order_slots": CAPACITY_DIRECT_SLOTS,
                "fallback_buckets": CAPACITY_FALLBACK_BUCKETS,
                "price_page_capacity": CAPACITY_PRICE_PAGES,
                "effective_mapped_bytes": 2000,
                "effective_direct_orders_mapped_bytes": 1000,
                "effective_price_pages_mapped_bytes": 1000,
                "effective_descriptor_bytes": 1000,
                "effective_storage_bytes": 3000,
                "price_pages": 50,
                **CAPACITY_REPLAY_IDENTITY,
            }
        )
    if correctness:
        main.update(
            {
                "mutation_digest": 111 if role == "baseline" else 222,
                "semantic_mutation_digest_schema": SEMANTIC_SCHEMA,
                "semantic_mutation_digest": 999,
            }
        )
    lines = [
        "itch_book_replay_ready " + " ".join(f"{key}={value}" for key, value in ready.items()),
        "itch_book_replay " + " ".join(f"{key}={value}" for key, value in main.items()),
    ]
    for message_type in MESSAGE_TYPES:
        values = per_type[message_type]
        fields = {
            "type": message_type,
            "sample_count": TYPE_SAMPLES[message_type],
            **values,
        }
        lines.append(
            "itch_book_replay_type "
            + " ".join(f"{key}={value}" for key, value in fields.items())
        )
    return "\n".join(lines) + "\n"


def memory_evidence(root: Path, prefix: str, role: str) -> None:
    write(root, f"{prefix}.command.txt", "benchmark --fixture\n")
    write(root, f"{prefix}.stderr.log")
    write(
        root,
        f"{prefix}.memory-evidence.txt",
        "monitor_affinity=cpu0\n"
        "post_prefault_snapshot=validated ready_marker=observed start_gate=released\n",
    )
    write(root, f"{prefix}.smaps_rollup.txt", "Rss: 100 kB\nSwap: 0 kB\n")
    write(root, f"{prefix}.smaps.txt", "fixture smaps\n")
    write(root, f"{prefix}.numa_maps.txt", "fixture numa maps\n")
    write(
        root,
        f"{prefix}.numa-summary.txt",
        "N0_all_pages=100\nN0_anon_pages=100\nanon_total_pages=100\n"
        "anon_selected_pages=100\nanon_other_pages=0\n",
    )
    write(root, f"{prefix}.start-gate")
    if role == "baseline":
        write(root, f"{prefix}.native-ranges.txt", native_manifest_text())


def make_manifest(root: Path) -> None:
    manifest = root / "manifest.sha256"
    if manifest.exists():
        manifest.unlink()
    entries: List[str] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = "./" + path.relative_to(root).as_posix()
        entries.append(f"{digest(path.read_bytes())}  {relative}\n")
    manifest.write_text("".join(entries), encoding="ascii")


def replace_equals_field(path: Path, key: str, value: Union[str, int]) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    matches = [index for index, line in enumerate(lines) if line.startswith(f"{key}=")]
    if len(matches) != 1:
        raise AssertionError(f"{path} has {len(matches)} {key}= fields")
    lines[matches[0]] = f"{key}={value}"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def refresh_source_build_attestation_hash(root: Path) -> None:
    attestation = root / "provenance/build/source-build/source-build-attestation.txt"
    attestation_sha = digest(attestation.read_bytes())
    replace_equals_field(
        root / "preflight.txt", "source_build_attestation_sha256", attestation_sha
    )
    provenance = root / "provenance/provenance-verification.txt"
    replace_equals_field(
        provenance, "source_build_attestation_sha256_before", attestation_sha
    )
    replace_equals_field(
        provenance, "source_build_attestation_sha256_after", attestation_sha
    )


def make_artifact(
    root: Path,
    role: str,
    *,
    p50_failure: bool = False,
    tail_regression: bool = False,
    trace_sha: str = TRACE_SHA256,
    dirty: bool = False,
    runs: int = 5,
) -> None:
    branch = BASELINE_BRANCH if role == "baseline" else CANDIDATE_BRANCH
    commit = BASELINE_COMMIT if role == "baseline" else CANDIDATE_COMMIT
    binary = (f"{role} binary\n").encode()
    harness = b"same acceptance harness\n"
    verifier = b"same hot path verifier\n"
    cache = b"same cmake cache\n"
    fingerprint = ("b" if role == "baseline" else "c") * 64
    parent = BRANCH5_PINNED_COMMIT if role == "baseline" else "3" * 40
    tree = BRANCH5_COMPATIBILITY_TREE if role == "baseline" else "4" * 40
    binary_sha = digest(binary)
    harness_sha = digest(harness)
    verifier_sha = digest(verifier)
    cache_sha = digest(cache)
    source_archive = (f"{role} clean source archive\n").encode()
    source_archive_sha = digest(source_archive)
    source_root = f"/fixture/source/{role}"
    source_build_root = f"/tmp/astra-order-book-source-build.{role}"
    source_build_source = f"{source_build_root}/source"
    source_build_directory = f"{source_build_root}/build"
    source_archive_output = (
        root / "provenance/build/source-build/source-tree.tar"
    ).as_posix()
    git_command = "/usr/bin/git"
    tar_command = "/usr/bin/tar"
    cmake_command = "/usr/bin/cmake"
    env_command = "/usr/bin/env"
    cmake_make_program = "/usr/bin/ninja"
    cmake_cxx_compiler = "/usr/bin/c++"
    cmake_cxx_flags = ""
    cmake_cxx_flags_release = "-O3 -DNDEBUG"
    cmake_exe_linker_flags = ""
    cmake_exe_linker_flags_release = ""
    cmake_static_linker_flags = ""
    cmake_static_linker_flags_release = ""
    source_epoch = "1700000000"
    fresh_cache = (
        f"CMAKE_HOME_DIRECTORY:INTERNAL={source_build_source}\n"
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "CMAKE_GENERATOR:INTERNAL=Ninja\n"
        f"CMAKE_MAKE_PROGRAM:FILEPATH={cmake_make_program}\n"
        f"CMAKE_CXX_COMPILER:FILEPATH={cmake_cxx_compiler}\n"
        f"CMAKE_CXX_FLAGS:STRING={cmake_cxx_flags}\n"
        f"CMAKE_CXX_FLAGS_RELEASE:STRING={cmake_cxx_flags_release}\n"
        f"CMAKE_EXE_LINKER_FLAGS:STRING={cmake_exe_linker_flags}\n"
        f"CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING={cmake_exe_linker_flags_release}\n"
        f"CMAKE_STATIC_LINKER_FLAGS:STRING={cmake_static_linker_flags}\n"
        f"CMAKE_STATIC_LINKER_FLAGS_RELEASE:STRING={cmake_static_linker_flags_release}\n"
        "ASTRA_BUILD_APPS:BOOL=OFF\n"
        "ASTRA_BUILD_TESTS:BOOL=OFF\n"
        "ASTRA_BUILD_BENCHMARKS:BOOL=ON\n"
        "ASTRA_ENABLE_DPDK:BOOL=OFF\n"
        "ASTRA_ENABLE_IPO:UNINITIALIZED=OFF\n"
    ).encode()
    fresh_cache_sha = digest(fresh_cache)
    environment_prefix = [
        env_command,
        "-i",
        f"HOME={source_build_root}/home",
        "PATH=/usr/bin:/bin",
        f"TMPDIR={source_build_root}/tmp",
        "LC_ALL=C",
        f"SOURCE_DATE_EPOCH={source_epoch}",
        "ZERO_AR_DATE=1",
        cmake_command,
    ]
    source_archive_argv = [
        git_command,
        "-C",
        source_root,
        "archive",
        "--format=tar",
        f"--output={source_archive_output}",
        commit,
    ]
    source_extract_argv = [
        tar_command,
        "-xf",
        source_archive_output,
        "-C",
        source_build_source,
    ]
    configure_argv = environment_prefix + [
        "-S",
        source_build_source,
        "-B",
        source_build_directory,
        "-G",
        "Ninja",
        "--no-warn-unused-cli",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_MAKE_PROGRAM={cmake_make_program}",
        f"-DCMAKE_CXX_COMPILER={cmake_cxx_compiler}",
        f"-DCMAKE_CXX_FLAGS={cmake_cxx_flags}",
        f"-DCMAKE_CXX_FLAGS_RELEASE={cmake_cxx_flags_release}",
        f"-DCMAKE_EXE_LINKER_FLAGS={cmake_exe_linker_flags}",
        f"-DCMAKE_EXE_LINKER_FLAGS_RELEASE={cmake_exe_linker_flags_release}",
        f"-DCMAKE_STATIC_LINKER_FLAGS={cmake_static_linker_flags}",
        f"-DCMAKE_STATIC_LINKER_FLAGS_RELEASE={cmake_static_linker_flags_release}",
        "-DASTRA_BUILD_APPS=OFF",
        "-DASTRA_BUILD_TESTS=OFF",
        "-DASTRA_BUILD_BENCHMARKS=ON",
        "-DASTRA_ENABLE_DPDK=OFF",
        "-DASTRA_ENABLE_IPO=OFF",
    ]
    target_build_argv = environment_prefix + [
        "--build",
        source_build_directory,
        "--target",
        "astra_itch_book_replay_benchmark",
        "--clean-first",
        "--verbose",
    ]
    source_archive_argv_data = argv_bytes(source_archive_argv)
    source_extract_argv_data = argv_bytes(source_extract_argv)
    configure_argv_data = argv_bytes(configure_argv)
    target_build_argv_data = argv_bytes(target_build_argv)
    preflight = {
        "artifact_schema": "astra_order_book_acceptance_v1",
        "host": "ec2-fixture",
        "kernel": "Linux fixture x86_64",
        "boot_id": "12345678-1234-1234-1234-123456789abc",
        "machine": "x86_64",
        "cpu_vendor_id": "GenuineIntel",
        "cpu_family": 6,
        "cpu_model": 85,
        "cpu_stepping": 7,
        "cpu_model_name": "Fixture CPU",
        "cpu_online_list": "0-7",
        "cpu_isolated_list": "2",
        "cpu_isolation_source": "sysfs_domain_isolated",
        "cpu_scaling_governor": "performance",
        "cpu_scaling_driver": "intel_pstate",
        "binary_sha256": binary_sha,
        "binary_archive_sha256": binary_sha,
        "source_build_attestation_version": 1,
        "source_build_mode": "git_archive_fresh_cmake_clean_first_v1",
        "source_build_target": "astra_itch_book_replay_benchmark",
        "source_build_attestation_sha256": "pending",
        "source_tree_archive_sha256": source_archive_sha,
        "fresh_binary_sha256": binary_sha,
        "fresh_binary_archive_sha256": binary_sha,
        "trace_size_bytes": EXPECTED_BYTES,
        "trace_sha256": trace_sha,
        "trace_hash_policy": "mandatory_pre_and_post",
        "harness_source_sha256": harness_sha,
        "harness_archive_sha256": harness_sha,
        "hot_path_verifier_source_sha256": verifier_sha,
        "hot_path_verifier_archive_sha256": verifier_sha,
        "git_dirty": 1 if dirty else 0,
        "git_commit_before": commit,
        "git_parent_before": parent,
        "git_tree_before": tree,
        "git_branch_before": branch,
        "git_fingerprint_before": fingerprint,
        "cmake_build_type": "Release",
        "cmake_generator": "Ninja",
        "cmake_cxx_compiler": "/usr/bin/c++",
        "cmake_cxx_compiler_id": "GNU",
        "cmake_cxx_compiler_version": "13.2.0",
        "runs": runs,
        "cpu": 2,
        "monitor_cpu": 0,
        "numa_node": 0,
        "node_cpulist": "0-7",
        "node_mem_total_bytes": 250000000000,
        "expected_records": EXPECTED_RECORDS,
        "expected_bytes": EXPECTED_BYTES,
        "sample_every": 64,
        "warmup_book_messages": 1000000,
        "min_samples": 100,
        "plan_hot_arena_schema": (
            BRANCH5_SCHEMA if role == "baseline" else REDESIGN_SCHEMA
        ),
        "plan_hot_arena_policy": (
            "branch5_native_ranges_v1"
            if role == "baseline"
            else "redesign_exact_v1"
        ),
        "allow_swap": 0,
        "run_perf_stat": 1,
        "perf_version": "perf version 6.8.0",
        "swap_total_kb": 0,
        "vm_overcommit_memory": 0,
        "vm_overcommit_ratio": 50,
        "thp_enabled": "always [madvise] never",
        "thp_defrag": "always defer [defer+madvise] madvise never",
        "cgroup_memory_version": "v2",
        "cgroup_memory_dir": "/sys/fs/cgroup/fixture",
        "cgroup_finite_limit_count": 1,
        "cgroup_effective_max": 300000000000,
    }
    if role == "baseline":
        preflight.update(
            {
                "capacity_profile_policy": "not_applicable_branch5_native_v1",
                **{
                    field: "not_applicable"
                    for field in CAPACITY_REPLAY_FIELD_NAMES
                },
                "capacity_evidence_source_sha256": "not_applicable",
                "capacity_evidence_archive_sha256": "not_applicable",
                "admission_basis": "explicit_override_plus_reserve",
                "branch5_capacity_policy": "default_binary_unbound_trace_v1",
                "branch5_capacity_profile_name": "branch5-default-binary-v1",
                "branch5_capacity_evidence_policy": "unbound_trace_v1",
                "branch5_runtime_capacity_mode": "binary_defaults_v1",
                "sample_capacity_override": "binary-default",
                "branch5_plan_scope": "shared_price_pool_only",
                "branch5_per_book_bytes_resolved_at_ready": 1,
                "derived_planned_storage_bytes": BRANCH5_PRICE_POOL_BYTES,
                "planned_bytes": 3 * 1024 * 1024 * 1024,
                "reserve_bytes": BRANCH5_RESERVE_BYTES,
                "required_bytes": (
                    3 * 1024 * 1024 * 1024 + BRANCH5_RESERVE_BYTES
                ),
                "plan_price_pool_bytes": BRANCH5_PRICE_POOL_BYTES,
                **{
                    f"plan_{field}": value
                    for field, value in BRANCH5_PRICE_VECTOR_BYTES.items()
                },
                **{
                    f"plan_{field}": value
                    for field, value in BRANCH5_PLAN_CAPACITIES.items()
                },
            }
        )
    else:
        preflight.update(
            {
                "capacity_profile_policy": "canonical_manifest_v1",
                **CAPACITY_REPLAY_IDENTITY,
                "capacity_evidence_source_sha256": CAPACITY_EVIDENCE_SHA256,
                "capacity_evidence_archive_sha256": CAPACITY_EVIDENCE_SHA256,
            }
        )
    build = {
        "cmake_build_type": "Release",
        "cmake_generator": "Ninja",
        "cmake_make_program": cmake_make_program,
        "cmake_cxx_compiler": cmake_cxx_compiler,
        "cmake_cxx_compiler_id": "GNU",
        "cmake_cxx_compiler_version": "13.2.0",
        "cmake_cxx_flags": "''",
        "cmake_cxx_flags_release": "-O3\\ -DNDEBUG",
        "cmake_exe_linker_flags": "''",
        "cmake_exe_linker_flags_release": "''",
        "cmake_static_linker_flags": "''",
        "cmake_static_linker_flags_release": "''",
        "source_build_attestation_version": 1,
        "source_build_mode": "git_archive_fresh_cmake_clean_first_v1",
        "source_build_environment_policy": "empty_environment_recorded_toolchain_v1",
        "source_build_target": "astra_itch_book_replay_benchmark",
    }
    source_build = {
        "attestation_version": 1,
        "mode": "git_archive_fresh_cmake_clean_first_v1",
        "environment_policy": "empty_environment_recorded_toolchain_v1",
        "target": "astra_itch_book_replay_benchmark",
        "source_commit": commit,
        "source_parent": parent,
        "source_tree": tree,
        "source_branch": branch,
        "source_fingerprint_before": fingerprint,
        "source_fingerprint_after": fingerprint,
        "source_date_epoch": source_epoch,
        "git_command": git_command,
        "tar_command": tar_command,
        "cmake_command": cmake_command,
        "env_command": env_command,
        "source_root": source_root,
        "source_archive_output": source_archive_output,
        "temporary_root": source_build_root,
        "temporary_source": source_build_source,
        "temporary_build": source_build_directory,
        "source_archive_sha256": source_archive_sha,
        "source_archive_argv_sha256": digest(source_archive_argv_data),
        "source_extract_argv_sha256": digest(source_extract_argv_data),
        "configure_argv_sha256": digest(configure_argv_data),
        "target_build_argv_sha256": digest(target_build_argv_data),
        "fresh_cmake_cache_sha256": fresh_cache_sha,
        "supplied_binary_sha256": binary_sha,
        "fresh_binary_sha256": binary_sha,
        "fresh_binary_archive_sha256": binary_sha,
        "configure_exit_code": 0,
        "target_build_exit_code": 0,
        "result": "PASS",
        "error": "none",
    }
    source_build_text = equals_text(source_build)
    source_build_sha = digest(source_build_text.encode("utf-8"))
    preflight["source_build_attestation_sha256"] = source_build_sha
    provenance = {
        "provenance_version": 2,
        "binary_sha256_before": binary_sha,
        "binary_sha256_after": binary_sha,
        "binary_archive_sha256_before": binary_sha,
        "binary_archive_sha256_after": binary_sha,
        "trace_sha256_before": trace_sha,
        "trace_sha256_after": trace_sha,
        "capacity_evidence_sha256_before": (
            "not_applicable" if role == "baseline" else CAPACITY_EVIDENCE_SHA256
        ),
        "capacity_evidence_sha256_after": (
            "not_applicable" if role == "baseline" else CAPACITY_EVIDENCE_SHA256
        ),
        "capacity_evidence_archive_sha256_before": (
            "not_applicable" if role == "baseline" else CAPACITY_EVIDENCE_SHA256
        ),
        "capacity_evidence_archive_sha256_after": (
            "not_applicable" if role == "baseline" else CAPACITY_EVIDENCE_SHA256
        ),
        "harness_sha256_before": harness_sha,
        "harness_sha256_after": harness_sha,
        "harness_archive_sha256_before": harness_sha,
        "harness_archive_sha256_after": harness_sha,
        "hot_path_verifier_sha256_before": verifier_sha,
        "hot_path_verifier_sha256_after": verifier_sha,
        "cmake_cache_sha256_before": cache_sha,
        "cmake_cache_sha256_after": cache_sha,
        "source_build_attestation_sha256_before": source_build_sha,
        "source_build_attestation_sha256_after": source_build_sha,
        "source_tree_archive_sha256_before": source_archive_sha,
        "source_tree_archive_sha256_after": source_archive_sha,
        "fresh_binary_archive_sha256_before": binary_sha,
        "fresh_binary_archive_sha256_after": binary_sha,
        "fresh_cmake_cache_sha256_before": fresh_cache_sha,
        "fresh_cmake_cache_sha256_after": fresh_cache_sha,
        "git_commit_before": commit,
        "git_commit_after": commit,
        "git_parent_before": parent,
        "git_parent_after": parent,
        "git_tree_before": tree,
        "git_tree_after": tree,
        "git_branch_before": branch,
        "git_branch_after": branch,
        "git_fingerprint_before": fingerprint,
        "git_fingerprint_after": fingerprint,
        "result": "PASS",
        "error": "none",
    }
    write(root, "preflight.txt", equals_text(preflight))
    write(root, "provenance/build/build-provenance.txt", equals_text(build))
    write(root, "provenance/build/compiler-version.txt", "fixture c++ 13.2.0\n")
    write(root, "provenance/build/CMakeCache.txt", cache)
    source_build_prefix = "provenance/build/source-build"
    write(root, f"{source_build_prefix}/source-build-attestation.txt", source_build_text)
    write(root, f"{source_build_prefix}/source-tree.tar", source_archive)
    write(root, f"{source_build_prefix}/fresh-built-binary", binary)
    write(root, f"{source_build_prefix}/CMakeCache.txt", fresh_cache)
    for state_name in (
        "source-build-attestation-before.state",
        "source-build-attestation-after.state",
        "source-tree-archive-before.state",
        "source-tree-archive-after.state",
        "fresh-binary-archive-before.state",
        "fresh-binary-archive-after.state",
        "cmake-cache-archive-before.state",
        "cmake-cache-archive-after.state",
    ):
        write(root, f"{source_build_prefix}/{state_name}", "fixture state\n")
    for name, arguments, data in (
        ("source-archive", source_archive_argv, source_archive_argv_data),
        ("source-extract", source_extract_argv, source_extract_argv_data),
        ("configure", configure_argv, configure_argv_data),
        ("target-build", target_build_argv, target_build_argv_data),
    ):
        write(root, f"{source_build_prefix}/{name}.command.txt", shlex.join(arguments) + "\n")
        write(root, f"{source_build_prefix}/{name}.argv", data)
        write(root, f"{source_build_prefix}/{name}.stdout.log", f"fixture {name} stdout\n")
        write(root, f"{source_build_prefix}/{name}.stderr.log")
    write(root, "provenance/tested-binary", binary)
    write(root, "provenance/run_order_book_acceptance.sh", harness)
    write(root, "provenance/verify_order_book_hot_path.sh", verifier)
    write(root, "provenance/provenance-verification.txt", equals_text(provenance))
    if role == "candidate":
        write(
            root,
            "provenance/capacity-evidence-manifest.txt",
            CAPACITY_MANIFEST_BYTES,
        )
        capacity_stat = (
            f"device=1 inode=2 mode=81a4 size={len(CAPACITY_MANIFEST_BYTES)} "
            "mtime=fixture ctime=fixture"
        )
        source_state = equals_text(
            {
                "path": "/fixture/capacity-evidence.txt",
                "sha256": CAPACITY_EVIDENCE_SHA256,
                "stat_before_hash": capacity_stat,
                "stat_after_hash": capacity_stat,
                "stable_during_capture": 1,
            }
        )
        archive_state = equals_text(
            {
                "path": (
                    root / "provenance/capacity-evidence-manifest.txt"
                ).as_posix(),
                "sha256": CAPACITY_EVIDENCE_SHA256,
                "stat_before_hash": capacity_stat,
                "stat_after_hash": capacity_stat,
                "stable_during_capture": 1,
            }
        )
        for state_name in (
            "capacity-evidence-source-before.state",
            "capacity-evidence-source-after.state",
        ):
            write(root, f"provenance/{state_name}", source_state)
        for state_name in (
            "capacity-evidence-archive-before.state",
            "capacity-evidence-archive-after.state",
        ):
            write(root, f"provenance/{state_name}", archive_state)
    write(
        root,
        "provenance/hot-path-verifier.stdout.log",
        f"hot_path_verifier version=2 result=PASS schema="
        f"{BRANCH5_SCHEMA if role == 'baseline' else REDESIGN_SCHEMA} "
        f"binary_sha256={binary_sha} "
        "selected_functions=85 forbidden_targets=0 indirect_calls=0 lock_prefixes=0\n",
    )
    write(root, "provenance/hot-path-disassembly.txt", "fixture disassembly\n")
    write(root, "git-status-porcelain.txt")
    for phase in ("before", "post-build", "after"):
        write(root, f"provenance/git-commit-{phase}.txt", commit + "\n")
        write(root, f"provenance/git-parent-{phase}.txt", parent + "\n")
        write(root, f"provenance/git-tree-{phase}.txt", tree + "\n")
        write(root, f"provenance/git-branch-{phase}.txt", branch + "\n")
        for relative in (
            f"provenance/git-status-{phase}.txt",
            f"provenance/git-diff-head-{phase}.patch",
            f"provenance/git-untracked-paths-{phase}.nul",
            f"provenance/git-untracked-blobs-{phase}.txt",
        ):
            write(root, relative)

    storage_plan = (
        "itch_book_replay_storage_plan hot_arena_schema="
        + (BRANCH5_SCHEMA if role == "baseline" else REDESIGN_SCHEMA)
    )
    if role == "baseline":
        storage_plan += (
            f" planned_price_pool_bytes={BRANCH5_PRICE_POOL_BYTES} "
            + " ".join(
                f"{key}={value}"
                for key, value in BRANCH5_PRICE_VECTOR_BYTES.items()
            )
            + " "
            + " ".join(
                f"{key}={value}"
                for key, value in BRANCH5_PLAN_CAPACITIES.items()
            )
        )
    else:
        storage_plan += " " + " ".join(
            f"{key}={value}" for key, value in CAPACITY_REPLAY_IDENTITY.items()
        )
        storage_plan += (
            f" direct_order_slots={CAPACITY_DIRECT_SLOTS}"
            f" fallback_buckets={CAPACITY_FALLBACK_BUCKETS}"
            f" price_page_capacity={CAPACITY_PRICE_PAGES}"
        )
    storage_plan += "\n"
    for relative, data in (
        (
            "storage-plan.stdout.log",
            storage_plan,
        ),
        ("storage-plan.stderr.log", ""),
        ("storage-plan.command.txt", "benchmark --storage-plan-only\n"),
        ("benchmark-help.txt", "fixture help\n"),
        ("perf-preflight.perf-stat.csv", "1;cycles\n"),
        ("hardware-counters.command.txt", "perf stat fixture\n"),
        ("hardware-counters.stdout.log", "fixture hardware replay\n"),
        ("hardware-counters.stderr.log", ""),
        ("hardware-counters.perf-stat.csv", "1;cycles\n"),
    ):
        write(root, relative, data)

    summary_rows: List[str] = []
    all_run_metrics: List[Dict[str, int]] = []
    for run in range(1, runs + 1):
        run_id = f"{run:03d}"
        prefix = f"latency-{run_id}"
        memory_evidence(root, prefix, role)
        output = replay_output(
            role,
            run,
            correctness=False,
            p50_failure=p50_failure,
            tail_regression=tail_regression,
        )
        write(root, f"{prefix}.stdout.log", output)
        aggregate = metrics(role, "aggregate", run, tail_regression)
        if p50_failure and role == "candidate" and run == 2:
            aggregate["p50_ns"] = 151
        all_run_metrics.append(aggregate)
        summary_rows.append(
            f"latency\t{run_id}\tPASS\t0\t{aggregate['p50_ns']}\t{aggregate['p99_ns']}\t"
            f"{aggregate['p99_9_ns']}\tsample_count=100\t{root}/{prefix}.stdout.log\t"
            f"{root}/{prefix}.stderr.log\n"
        )

    prefix = "correctness-verification"
    memory_evidence(root, prefix, role)
    write(root, f"{prefix}.stdout.log", replay_output(role, 1, correctness=True))
    write(
        root,
        f"{prefix}.command.txt",
        "benchmark --expect-semantic-mutation-digest=999\n",
    )
    summary_rows.append(
        "correctness\tcorrectness-verification\tPASS\t0\t-\t-\t-\t"
        f"mutation_digest=fixture semantic_mutation_digest=999\t{root}/{prefix}.stdout.log\t"
        f"{root}/{prefix}.stderr.log\n"
    )
    summary_rows.extend(
        (
            "hardware-counters\tperf-stat\tPASS\t0\t-\t-\t-\tevents=fixture\t"
            f"{root}/hardware-counters.stdout.log\t{root}/hardware-counters.stderr.log\n",
            "provenance\tpre-post\tPASS\t0\t-\t-\t-\tstable\t"
            f"{root}/provenance/provenance-verification.txt\t-\n",
            f"worst\tall-latency-runs\t-\t-\t{max(item['p50_ns'] for item in all_run_metrics)}\t"
            f"{max(item['p99_ns'] for item in all_run_metrics)}\t"
            f"{max(item['p99_9_ns'] for item in all_run_metrics)}\tceilings\t-\t-\n",
            "overall\t-\tPASS\t0\t-\t-\t-\tall requested runs passed\t-\t-\n",
        )
    )
    write(root, "summary.tsv", SUMMARY_HEADER + "".join(summary_rows))
    make_manifest(root)


def bind_pinned_branch5_capacity_evidence(root: Path) -> None:
    evidence_source = (
        ROOT / "compat/branch5_native_v1/capacity-evidence-01302019.txt"
    )
    evidence = evidence_source.read_bytes()
    if digest(evidence) != BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256:
        raise AssertionError("reviewed branch5 capacity evidence hash changed")

    preflight = root / "preflight.txt"
    for field, value in {
        "capacity_evidence_source_sha256": (
            BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256
        ),
        "capacity_evidence_archive_sha256": (
            BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256
        ),
        "branch5_capacity_policy": "pinned_trace_canonical_v1",
        "branch5_capacity_profile_name": BRANCH5_PINNED_CAPACITY_PROFILE,
        "branch5_capacity_evidence_policy": "reviewed_manifest_sha256_v1",
        "branch5_runtime_capacity_mode": "canonical_explicit_v1",
        "sample_capacity_override": 8388608,
        "planned_bytes": BRANCH5_PLANNED_BYTES,
        "required_bytes": BRANCH5_PLANNED_BYTES + BRANCH5_RESERVE_BYTES,
    }.items():
        replace_equals_field(preflight, field, value)

    provenance = root / "provenance/provenance-verification.txt"
    for field in (
        "capacity_evidence_sha256_before",
        "capacity_evidence_sha256_after",
        "capacity_evidence_archive_sha256_before",
        "capacity_evidence_archive_sha256_after",
    ):
        replace_equals_field(
            provenance, field, BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256
        )

    archive = root / "provenance/capacity-evidence-manifest.txt"
    write(root, "provenance/capacity-evidence-manifest.txt", evidence)
    capture_stat = (
        f"device=1 inode=2 mode=81a4 size={len(evidence)} "
        "mtime=fixture ctime=fixture"
    )
    source_state = equals_text(
        {
            "path": evidence_source.as_posix(),
            "sha256": BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256,
            "stat_before_hash": capture_stat,
            "stat_after_hash": capture_stat,
            "stable_during_capture": 1,
        }
    )
    archive_state = equals_text(
        {
            "path": archive.as_posix(),
            "sha256": BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256,
            "stat_before_hash": capture_stat,
            "stat_after_hash": capture_stat,
            "stable_during_capture": 1,
        }
    )
    for state_name in (
        "capacity-evidence-source-before.state",
        "capacity-evidence-source-after.state",
    ):
        write(root, f"provenance/{state_name}", source_state)
    for state_name in (
        "capacity-evidence-archive-before.state",
        "capacity-evidence-archive-after.state",
    ):
        write(root, f"provenance/{state_name}", archive_state)
    write(
        root,
        "storage-plan.command.txt",
        "benchmark --storage-plan-only "
        "--default-order-capacity=65536 "
        "--price-node-capacity=163840 "
        "--price-leaf-capacity=1048576 "
        "--price-level-capacity=2097152 "
        "--sample-capacity=8388608\n",
    )
    for replay in (
        *root.glob("latency-*.stdout.log"),
        *root.glob("correctness-*.stdout.log"),
    ):
        replay.write_text(
            replay.read_text(encoding="utf-8").replace(
                "sample_capacity=1000", "sample_capacity=8388608"
            ),
            encoding="utf-8",
        )
    make_manifest(root)


class ComparatorFixtures(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="astra-comparator-test-")
        self.root = Path(self.temporary.name)
        self.baseline = self.root / "baseline"
        self.candidate = self.root / "candidate"
        self.baseline.mkdir()
        self.candidate.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def command(
        self,
        expected_capacity_sha256: str = CAPACITY_EVIDENCE_SHA256,
        expected_trace_sha256: str = TRACE_SHA256,
    ) -> List[str]:
        return [
            str(COMPARATOR),
            "--baseline-dir",
            str(self.baseline),
            "--candidate-dir",
            str(self.candidate),
            "--expect-baseline-branch",
            BASELINE_BRANCH,
            "--expect-baseline-commit",
            BASELINE_COMMIT,
            "--expect-candidate-branch",
            CANDIDATE_BRANCH,
            "--expect-candidate-commit",
            CANDIDATE_COMMIT,
            "--expect-candidate-capacity-evidence-sha256",
            expected_capacity_sha256,
            "--expect-trace-sha256",
            expected_trace_sha256,
            "--expect-records",
            str(EXPECTED_RECORDS),
            "--expect-bytes",
            str(EXPECTED_BYTES),
        ]

    def run_comparator(
        self,
        expected_capacity_sha256: str = CAPACITY_EVIDENCE_SHA256,
        expected_trace_sha256: str = TRACE_SHA256,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            self.command(expected_capacity_sha256, expected_trace_sha256),
            text=True,
            capture_output=True,
            check=False,
        )

    def make_pair(self, **candidate_options: object) -> None:
        make_artifact(self.baseline, "baseline")
        make_artifact(self.candidate, "candidate", **candidate_options)

    def test_valid_pair_passes_and_emits_all_normalized_rows(self) -> None:
        self.make_pair()
        result = self.run_comparator()
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [line for line in result.stdout.splitlines() if line[:3].isdigit()]
        self.assertEqual(len(rows), 5 * 8 * 5)
        self.assertIn("comparison\tresult\tPASS", result.stdout)
        self.assertIn("candidate_over_baseline", result.stdout)

    def test_more_than_five_runs_compares_every_run(self) -> None:
        make_artifact(self.baseline, "baseline", runs=6)
        make_artifact(self.candidate, "candidate", runs=6)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [line for line in result.stdout.splitlines() if line[:3].isdigit()]
        self.assertEqual(len(rows), 6 * 8 * 5)
        self.assertIn("baseline worst value across all 6 runs", result.stdout)

    def test_fewer_than_five_runs_is_rejected(self) -> None:
        make_artifact(self.baseline, "baseline", runs=4)
        make_artifact(self.candidate, "candidate", runs=4)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("at least five latency runs", result.stderr)

    def test_manifest_detects_tampering(self) -> None:
        self.make_pair()
        with (self.candidate / "latency-003.stdout.log").open("a", encoding="utf-8") as output:
            output.write("tampered\n")
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("SHA-256 mismatch", result.stderr)

    def test_missing_candidate_capacity_manifest_is_rejected(self) -> None:
        self.make_pair()
        (
            self.candidate / "provenance/capacity-evidence-manifest.txt"
        ).unlink()
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("required evidence is missing", result.stderr)

    def test_noncanonical_capacity_manifest_is_rejected_after_outer_rehash(
        self,
    ) -> None:
        self.make_pair()
        manifest = self.candidate / "provenance/capacity-evidence-manifest.txt"
        manifest.write_bytes(
            CAPACITY_MANIFEST_BYTES.replace(
                b"order_direct_slots=128\n",
                b"order_direct_slots=0128\n",
            )
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("canonical unsigned integer", result.stderr)

    def test_forged_capacity_capture_state_is_rejected_after_outer_rehash(
        self,
    ) -> None:
        self.make_pair()
        write(
            self.candidate,
            "provenance/capacity-evidence-source-before.state",
            "fixture state\n",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing required fields", result.stderr)

    def test_storage_plan_capacity_must_match_authenticated_manifest(self) -> None:
        self.make_pair()
        storage_plan = self.candidate / "storage-plan.stdout.log"
        storage_plan.write_text(
            storage_plan.read_text(encoding="utf-8").replace(
                f"direct_order_slots={CAPACITY_DIRECT_SLOTS}",
                f"direct_order_slots={CAPACITY_DIRECT_SLOTS + 1}",
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("planned direct table is not bound", result.stderr)

    def test_final_capacity_must_match_authenticated_storage_plan(self) -> None:
        self.make_pair()
        replay = self.candidate / "latency-001.stdout.log"
        replay.write_text(
            replay.read_text(encoding="utf-8").replace(
                f"direct_order_slots={CAPACITY_DIRECT_SLOTS}",
                f"direct_order_slots={CAPACITY_DIRECT_SLOTS + 1}",
                1,
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "final direct_order_slots differs from authenticated storage plan",
            result.stderr,
        )

    def test_branch5_artifact_cannot_carry_redesign_capacity_evidence(self) -> None:
        self.make_pair()
        write(
            self.baseline,
            "provenance/capacity-evidence-manifest.txt",
            CAPACITY_MANIFEST_BYTES,
        )
        make_manifest(self.baseline)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "unexpectedly carries pinned capacity evidence", result.stderr
        )

    def test_branch5_native_payload_must_fit_planned_bytes(self) -> None:
        self.make_pair()
        planned = NATIVE_BYTES - 1
        replace_equals_field(
            self.baseline / "preflight.txt", "planned_bytes", planned
        )
        replace_equals_field(
            self.baseline / "preflight.txt",
            "required_bytes",
            planned + BRANCH5_RESERVE_BYTES,
        )
        make_manifest(self.baseline)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("payload exceeds the admitted planned bytes", result.stderr)

    def test_branch5_partial_explicit_capacity_flags_are_rejected(self) -> None:
        self.make_pair()
        replace_equals_field(
            self.baseline / "preflight.txt",
            "branch5_runtime_capacity_mode",
            "canonical_explicit_v1",
        )
        write(
            self.baseline,
            "storage-plan.command.txt",
            "benchmark --storage-plan-only "
            "--default-order-capacity=65536 "
            "--price-node-capacity=163840 "
            "--price-leaf-capacity=1048576\n",
        )
        make_manifest(self.baseline)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "capacity flags are partial or noncanonical", result.stderr
        )

    def test_branch5_noncanonical_storage_capacity_is_rejected(self) -> None:
        self.make_pair()
        replace_equals_field(
            self.baseline / "preflight.txt",
            "plan_default_order_capacity",
            65535,
        )
        storage_plan = self.baseline / "storage-plan.stdout.log"
        storage_plan.write_text(
            storage_plan.read_text(encoding="utf-8").replace(
                "default_order_capacity=65536",
                "default_order_capacity=65535",
            ),
            encoding="utf-8",
        )
        make_manifest(self.baseline)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("differs from the pinned capacity", result.stderr)

    def test_pinned_branch5_requires_reviewed_capacity_evidence(self) -> None:
        make_artifact(
            self.baseline,
            "baseline",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        make_artifact(
            self.candidate,
            "candidate",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        result = self.run_comparator(
            expected_trace_sha256=BRANCH5_PINNED_TRACE_SHA256
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("capacity_evidence_source_sha256", result.stderr)

    def test_pinned_branch5_rejects_modified_reviewed_evidence(self) -> None:
        make_artifact(
            self.baseline,
            "baseline",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        make_artifact(
            self.candidate,
            "candidate",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        bind_pinned_branch5_capacity_evidence(self.baseline)
        archive = self.baseline / "provenance/capacity-evidence-manifest.txt"
        archive.write_bytes(archive.read_bytes() + b"tampered=1\n")
        make_manifest(self.baseline)
        result = self.run_comparator(
            expected_trace_sha256=BRANCH5_PINNED_TRACE_SHA256
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("capacity manifest hash differs", result.stderr)

    def test_pinned_branch5_rejects_incomplete_book_universe(self) -> None:
        make_artifact(
            self.baseline,
            "baseline",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        make_artifact(
            self.candidate,
            "candidate",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        bind_pinned_branch5_capacity_evidence(self.baseline)
        result = self.run_comparator(
            expected_trace_sha256=BRANCH5_PINNED_TRACE_SHA256
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("pinned full-trace book universe", result.stderr)

    def test_pinned_branch5_rejects_noncanonical_reserve(self) -> None:
        make_artifact(
            self.baseline,
            "baseline",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        make_artifact(
            self.candidate,
            "candidate",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        bind_pinned_branch5_capacity_evidence(self.baseline)
        replace_equals_field(
            self.baseline / "preflight.txt",
            "reserve_bytes",
            BRANCH5_RESERVE_BYTES - 1,
        )
        replace_equals_field(
            self.baseline / "preflight.txt",
            "required_bytes",
            BRANCH5_PLANNED_BYTES + BRANCH5_RESERVE_BYTES - 1,
        )
        make_manifest(self.baseline)
        result = self.run_comparator(
            expected_trace_sha256=BRANCH5_PINNED_TRACE_SHA256
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("canonical reserve/sample plan", result.stderr)

    def test_pinned_branch5_rejects_noncanonical_sample_plan(self) -> None:
        make_artifact(
            self.baseline,
            "baseline",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        make_artifact(
            self.candidate,
            "candidate",
            trace_sha=BRANCH5_PINNED_TRACE_SHA256,
        )
        bind_pinned_branch5_capacity_evidence(self.baseline)
        replace_equals_field(
            self.baseline / "preflight.txt",
            "sample_capacity_override",
            8388607,
        )
        command = self.baseline / "storage-plan.command.txt"
        command.write_text(
            command.read_text(encoding="utf-8").replace(
                "--sample-capacity=8388608",
                "--sample-capacity=8388607",
            ),
            encoding="utf-8",
        )
        make_manifest(self.baseline)
        result = self.run_comparator(
            expected_trace_sha256=BRANCH5_PINNED_TRACE_SHA256
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("canonical reserve/sample plan", result.stderr)

    def test_pinned_trace_name_cannot_hide_capacity_substitution(self) -> None:
        self.make_pair()
        pinned_identity: Dict[str, Union[str, int]] = {
            **CAPACITY_REPLAY_IDENTITY,
            "capacity_evidence_schema": "astra_pinned_trace_capacity_evidence_v1",
            "capacity_profile_name": "nasdaq-itch-20190130-acceptance-v1",
            "capacity_evidence_sha256": TRACE_SHA256,
            "capacity_corpus_manifest_sha256": TRACE_SHA256,
            "capacity_profiler_sha256": (
                "c7f468bd3bc784398626997329f01653ac54b8691af822419931f23e95907956"
            ),
        }
        preflight = self.candidate / "preflight.txt"
        replace_equals_field(
            preflight,
            "capacity_profile_policy",
            "pinned_trace_builtin_v1",
        )
        for field, value in pinned_identity.items():
            replace_equals_field(preflight, field, value)

        provenance = self.candidate / "provenance/provenance-verification.txt"
        for field in (
            "capacity_evidence_sha256_before",
            "capacity_evidence_sha256_after",
            "capacity_evidence_archive_sha256_before",
            "capacity_evidence_archive_sha256_after",
        ):
            replace_equals_field(provenance, field, "not_applicable")

        for path in (
            self.candidate / "provenance/capacity-evidence-manifest.txt",
            *self.candidate.glob("provenance/capacity-evidence-*.state"),
        ):
            path.unlink()

        replay_paths = [self.candidate / "storage-plan.stdout.log"]
        replay_paths.extend(self.candidate.glob("*.stdout.log"))
        for path in replay_paths:
            text = path.read_text(encoding="utf-8")
            for field, value in pinned_identity.items():
                text = re.sub(
                    rf"(?<![A-Za-z0-9_]){re.escape(field)}=\S+",
                    f"{field}={value}",
                    text,
                )
            path.write_text(text, encoding="utf-8")

        make_manifest(self.candidate)
        result = self.run_comparator(TRACE_SHA256)
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "differs from the reviewed built-in profile",
            result.stderr,
        )

    def test_self_consistent_capacity_identity_substitution_needs_external_pin(
        self,
    ) -> None:
        self.make_pair()
        alternate_corpus_sha = "e" * 64
        alternate_manifest = CAPACITY_MANIFEST_BYTES.replace(
            CAPACITY_CORPUS_SHA256.encode("ascii"),
            alternate_corpus_sha.encode("ascii"),
        )
        alternate_evidence_sha = digest(alternate_manifest)
        (
            self.candidate / "provenance/capacity-evidence-manifest.txt"
        ).write_bytes(alternate_manifest)

        preflight = self.candidate / "preflight.txt"
        for field in (
            "capacity_evidence_sha256",
            "capacity_evidence_source_sha256",
            "capacity_evidence_archive_sha256",
        ):
            replace_equals_field(preflight, field, alternate_evidence_sha)
        replace_equals_field(
            preflight,
            "capacity_corpus_manifest_sha256",
            alternate_corpus_sha,
        )

        provenance = self.candidate / "provenance/provenance-verification.txt"
        for field in (
            "capacity_evidence_sha256_before",
            "capacity_evidence_sha256_after",
            "capacity_evidence_archive_sha256_before",
            "capacity_evidence_archive_sha256_after",
        ):
            replace_equals_field(provenance, field, alternate_evidence_sha)

        for state_path in self.candidate.glob(
            "provenance/capacity-evidence-*.state"
        ):
            state_path.write_text(
                state_path.read_text(encoding="utf-8").replace(
                    CAPACITY_EVIDENCE_SHA256,
                    alternate_evidence_sha,
                ),
                encoding="utf-8",
            )

        replay_paths = [self.candidate / "storage-plan.stdout.log"]
        replay_paths.extend(self.candidate.glob("*.stdout.log"))
        for path in replay_paths:
            text = path.read_text(encoding="utf-8")
            if CAPACITY_EVIDENCE_SHA256 not in text:
                continue
            path.write_text(
                text.replace(
                    CAPACITY_EVIDENCE_SHA256,
                    alternate_evidence_sha,
                ).replace(
                    CAPACITY_CORPUS_SHA256,
                    alternate_corpus_sha,
                ),
                encoding="utf-8",
            )

        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("explicit approved SHA-256", result.stderr)

    def test_recomputed_manifest_does_not_hide_trace_mismatch(self) -> None:
        make_artifact(self.baseline, "baseline")
        make_artifact(self.candidate, "candidate", trace_sha="d" * 64)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("trace_sha256 differs", result.stderr)

    def test_dirty_artifact_is_rejected(self) -> None:
        make_artifact(self.baseline, "baseline")
        make_artifact(self.candidate, "candidate", dirty=True)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("dirty worktree", result.stderr)

    def test_self_consistent_substituted_binary_is_rejected_without_fresh_build_match(
        self,
    ) -> None:
        self.make_pair()
        substituted = b"manually substituted stale binary\n"
        substituted_sha = digest(substituted)
        (self.candidate / "provenance/tested-binary").write_bytes(substituted)
        preflight = self.candidate / "preflight.txt"
        for field in ("binary_sha256", "binary_archive_sha256"):
            replace_equals_field(preflight, field, substituted_sha)
        provenance = self.candidate / "provenance/provenance-verification.txt"
        for field in (
            "binary_sha256_before",
            "binary_sha256_after",
            "binary_archive_sha256_before",
            "binary_archive_sha256_after",
        ):
            replace_equals_field(provenance, field, substituted_sha)
        verifier = self.candidate / "provenance/hot-path-verifier.stdout.log"
        verifier.write_text(
            re.sub(
                r"binary_sha256=[0-9a-f]{64}",
                f"binary_sha256={substituted_sha}",
                verifier.read_text(encoding="utf-8"),
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("fresh clean-source binary hashes", result.stderr)

    def test_forged_fresh_binary_attestation_field_is_rejected(self) -> None:
        self.make_pair()
        attestation = (
            self.candidate
            / "provenance/build/source-build/source-build-attestation.txt"
        )
        replace_equals_field(attestation, "fresh_binary_sha256", "d" * 64)
        refresh_source_build_attestation_hash(self.candidate)
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("fresh clean-source binary hashes", result.stderr)

    def test_fresh_build_command_must_name_reviewed_target(self) -> None:
        self.make_pair()
        argv_path = (
            self.candidate / "provenance/build/source-build/target-build.argv"
        )
        arguments = [
            argument.decode("utf-8")
            for argument in argv_path.read_bytes()[:-1].split(b"\0")
        ]
        target_index = arguments.index("--target") + 1
        arguments[target_index] = "unreviewed_replay_target"
        argv_path.write_bytes(argv_bytes(arguments))
        argv_path.with_name("target-build.command.txt").write_text(
            shlex.join(arguments) + "\n", encoding="utf-8"
        )
        attestation = (
            self.candidate
            / "provenance/build/source-build/source-build-attestation.txt"
        )
        replace_equals_field(
            attestation, "target_build_argv_sha256", digest(argv_path.read_bytes())
        )
        refresh_source_build_attestation_hash(self.candidate)
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("clean-first build of the replay target", result.stderr)

    def test_fresh_build_command_requires_clean_first(self) -> None:
        self.make_pair()
        argv_path = (
            self.candidate / "provenance/build/source-build/target-build.argv"
        )
        arguments = [
            argument.decode("utf-8")
            for argument in argv_path.read_bytes()[:-1].split(b"\0")
        ]
        arguments.remove("--clean-first")
        argv_path.write_bytes(argv_bytes(arguments))
        argv_path.with_name("target-build.command.txt").write_text(
            shlex.join(arguments) + "\n", encoding="utf-8"
        )
        attestation = (
            self.candidate
            / "provenance/build/source-build/source-build-attestation.txt"
        )
        replace_equals_field(
            attestation, "target_build_argv_sha256", digest(argv_path.read_bytes())
        )
        refresh_source_build_attestation_hash(self.candidate)
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("clean-first build of the replay target", result.stderr)

    def test_pre_fresh_build_provenance_schema_is_rejected(self) -> None:
        self.make_pair()
        replace_equals_field(
            self.candidate / "provenance/provenance-verification.txt",
            "provenance_version",
            1,
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown provenance verification schema", result.stderr)

    def test_missing_schedule_field_is_rejected_even_with_new_manifest(self) -> None:
        self.make_pair()
        path = self.candidate / "latency-001.stdout.log"
        text = path.read_text(encoding="utf-8").replace(
            f" sample_schedule_id={SCHEDULE_ID}", "", 1
        )
        path.write_text(text, encoding="utf-8")
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("sample_schedule_id", result.stderr)

    def test_post_system_s_boundary_must_match(self) -> None:
        self.make_pair()
        path = self.candidate / "latency-001.stdout.log"
        path.write_text(
            path.read_text(encoding="utf-8").replace(
                f"prelude_records={PRELUDE_RECORDS}",
                f"prelude_records={PRELUDE_RECORDS + 1}",
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("prelude_records differs", result.stderr)

    def test_effective_sample_capacity_must_match_between_artifacts(self) -> None:
        self.make_pair()
        path = self.candidate / "latency-001.stdout.log"
        path.write_text(
            path.read_text(encoding="utf-8").replace(
                "sample_capacity=1000", "sample_capacity=2000"
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "effective sample_capacity differs within or between artifacts",
            result.stderr,
        )

    def test_branch5_parent_and_tree_are_pinned(self) -> None:
        self.make_pair()
        replacement_parent = "5" * 40
        replacement_tree = "6" * 40
        for relative in (
            "preflight.txt",
            "provenance/provenance-verification.txt",
            "provenance/git-parent-before.txt",
            "provenance/git-parent-after.txt",
            "provenance/git-tree-before.txt",
            "provenance/git-tree-after.txt",
        ):
            path = self.baseline / relative
            text = path.read_text(encoding="utf-8")
            text = text.replace(BRANCH5_PINNED_COMMIT, replacement_parent)
            text = text.replace(BRANCH5_COMPATIBILITY_TREE, replacement_tree)
            path.write_text(text, encoding="utf-8")
        make_manifest(self.baseline)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("git-parent-post-build.txt differs", result.stderr)

    def test_branch5_reviewed_tree_is_required(self) -> None:
        self.make_pair()
        replacement_tree = "6" * 40
        for relative in (
            "preflight.txt",
            "provenance/provenance-verification.txt",
            "provenance/git-tree-before.txt",
            "provenance/git-tree-after.txt",
        ):
            path = self.baseline / relative
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    BRANCH5_COMPATIBILITY_TREE, replacement_tree
                ),
                encoding="utf-8",
            )
        make_manifest(self.baseline)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("git-tree-post-build.txt differs", result.stderr)

    def test_verifier_v1_artifact_is_rejected(self) -> None:
        self.make_pair()
        path = self.candidate / "provenance/hot-path-verifier.stdout.log"
        path.write_text(
            path.read_text(encoding="utf-8").replace("version=2", "version=1"),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("version-2 PASS", result.stderr)

    def test_candidate_p50_ceiling_is_an_independent_gate(self) -> None:
        self.make_pair(p50_failure=True)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("candidate_aggregate_p50_ceiling\t150\tFAIL", result.stdout)
        self.assertIn("comparison\tresult\tFAIL", result.stdout)

    def test_every_candidate_tail_must_beat_baseline_worst(self) -> None:
        self.make_pair(tail_regression=True)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("candidate_each_run_le_baseline_worst_all_runs", result.stdout)
        self.assertIn("comparison\tresult\tFAIL", result.stdout)

    def test_unlisted_extra_file_is_rejected(self) -> None:
        self.make_pair()
        write(self.candidate, "unlisted.txt", "extra\n")
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("manifest/file-set mismatch", result.stderr)

    def test_layout_independent_semantic_digest_must_match(self) -> None:
        self.make_pair()
        output_path = self.candidate / "correctness-verification.stdout.log"
        output_path.write_text(
            output_path.read_text(encoding="utf-8").replace(
                "semantic_mutation_digest=999", "semantic_mutation_digest=998"
            ),
            encoding="utf-8",
        )
        command_path = self.candidate / "correctness-verification.command.txt"
        command_path.write_text(
            command_path.read_text(encoding="utf-8").replace(
                "--expect-semantic-mutation-digest=999",
                "--expect-semantic-mutation-digest=998",
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("semantic mutation digests differ", result.stderr)

    def test_build_flags_must_match(self) -> None:
        self.make_pair()
        path = self.candidate / "provenance/build/build-provenance.txt"
        path.write_text(
            path.read_text(encoding="utf-8").replace(
                "cmake_cxx_flags_release=-O3",
                "cmake_cxx_flags_release=-O2",
            ),
            encoding="utf-8",
        )
        make_manifest(self.candidate)
        result = self.run_comparator()
        self.assertEqual(result.returncode, 1)
        self.assertIn("CMAKE_CXX_FLAGS_RELEASE differs", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
