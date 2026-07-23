#!/usr/bin/env python3
"""Fail-closed comparison of branch-5 and branch-6 acceptance artifacts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import re
import shlex
import shutil
import stat
import struct
import sys
import tempfile
from dataclasses import dataclass
from decimal import Decimal, localcontext
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Set, Tuple


MESSAGE_TYPES: Tuple[str, ...] = ("A", "F", "E", "C", "X", "D", "U")
WORKLOADS: Tuple[str, ...] = ("aggregate",) + MESSAGE_TYPES
METRICS: Tuple[str, ...] = ("p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "max_ns")
P50_LIMIT_NS = 150
SAMPLE_SCHEDULE_ID = "fixed_block_offset_v1_splitmix64_seed_61737472612d6974"
SEMANTIC_DIGEST_SCHEMA = "applied_itch_book_semantics_v1_fnv1a64le"
REDESIGN_SCHEMA = "redesign_v1"
BRANCH5_NATIVE_SCHEMA = "branch5_native_v1"
BRANCH5_NATIVE_MANIFEST_SCHEMA = "branch5_native_ranges_v1"
BRANCH5_PINNED_COMMIT = "324d81a15ee52cc72f68873a1ced122923406df2"
BRANCH5_COMPATIBILITY_TREE = "492938730e6db91e84bdb1f8e25152536e81dbc0"
BRANCH5_PINNED_TRACE_SHA256 = (
    "1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3"
)
BRANCH5_PINNED_CAPACITY_PROFILE = "nasdaq-itch-20190130-branch5-native-v1"
BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256 = (
    "05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282"
)
BRANCH5_PINNED_PREPARED_BOOKS = 8713
BRANCH5_PINNED_NATIVE_RANGE_COUNT = 34858
BRANCH5_PINNED_NATIVE_RANGE_BYTES = 62404952064
BRANCH5_PINNED_PLANNED_BYTES = 68719476736
BRANCH5_PINNED_RESERVE_BYTES = 17179869184
BRANCH5_PINNED_SAMPLE_EVERY = 64
BRANCH5_PINNED_SAMPLE_CAPACITY = 8388608
BRANCH5_PINNED_PRICE_POOL_BYTES = 2456420352
BRANCH5_PINNED_PLAN_FIELDS: Mapping[str, str] = {
    "default_order_capacity": "65536",
    "price_internal_node_capacity": "163840",
    "price_leaf_capacity": "1048576",
    "price_level_capacity": "2097152",
}
BRANCH5_PINNED_PRICE_VECTOR_FIELDS: Mapping[str, str] = {
    "price_nodes_bytes": "178257920",
    "price_leaves_bytes": "2214592512",
    "price_levels_bytes": "50331648",
    "price_free_nodes_bytes": "655360",
    "price_free_leaves_bytes": "4194304",
    "price_free_levels_bytes": "8388608",
}
BRANCH5_ORDER_CAPACITIES: Tuple[int, ...] = (
    65536,
    262144,
    1048576,
    4194304,
)
BRANCH5_ACTIVE_LOCATES = frozenset((85, 617, 1718, 5430, 5823, 6398))
BRANCH5_HOT_LOCATES = frozenset((14, 331, 381, 3419, 3420, 5217))
BRANCH5_ULTRA_HOT_LOCATES = frozenset((4118, 5628, 6449, 7291, 7804))
SUMMARY_HEADER: Tuple[str, ...] = (
    "kind",
    "id",
    "status",
    "exit_code",
    "p50_ns",
    "p99_ns",
    "p99_9_ns",
    "detail",
    "stdout",
    "stderr",
)

HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
UINT_RE = re.compile(r"^(?:0|[1-9][0-9]*)$")
SAFE_MANIFEST_PATH_RE = re.compile(r"^\./[A-Za-z0-9][A-Za-z0-9._/-]*$")
SAFE_BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
BOOT_ID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)
CAPACITY_PROFILE_RE = re.compile(r"^[A-Za-z0-9._:+/-]{1,128}$")
CAPACITY_MANIFEST_SCHEMA = "astra_book_capacity_evidence_v1"
PINNED_CAPACITY_SCHEMA = "astra_pinned_trace_capacity_evidence_v1"
PINNED_CAPACITY_PROFILE = "nasdaq-itch-20190130-acceptance-v1"
PINNED_CAPACITY_PROFILER_SHA256 = (
    "c7f468bd3bc784398626997329f01653ac54b8691af822419931f23e95907956"
)
PINNED_CAPACITY_REPLAY_FIELDS: Mapping[str, str] = {
    "capacity_profiled_max_order_ref": "329176641",
    "capacity_profiled_unique_price_pages": "68941",
    "capacity_minimum_direct_order_headroom": "207694270",
    "capacity_effective_direct_order_headroom": "207694270",
    "capacity_minimum_price_page_headroom": "11059",
    "capacity_effective_price_page_headroom": "11059",
}
PINNED_CAPACITY_PLAN_FIELDS: Mapping[str, str] = {
    "direct_order_slots": "536870912",
    "fallback_buckets": "1048576",
    "price_page_capacity": "80000",
}
CAPACITY_REPLAY_FIELDS: Tuple[str, ...] = (
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
CAPACITY_PREFLIGHT_FIELDS: Tuple[str, ...] = (
    "capacity_profile_policy",
) + CAPACITY_REPLAY_FIELDS + (
    "capacity_evidence_source_sha256",
    "capacity_evidence_archive_sha256",
)
BRANCH5_PREFLIGHT_FIELDS: Tuple[str, ...] = (
    "admission_basis",
    "branch5_capacity_policy",
    "branch5_capacity_profile_name",
    "branch5_capacity_evidence_policy",
    "branch5_runtime_capacity_mode",
    "sample_capacity_override",
    "branch5_plan_scope",
    "branch5_per_book_bytes_resolved_at_ready",
    "derived_planned_storage_bytes",
    "planned_bytes",
    "reserve_bytes",
    "required_bytes",
    "plan_price_pool_bytes",
    "plan_price_nodes_bytes",
    "plan_price_leaves_bytes",
    "plan_price_levels_bytes",
    "plan_price_free_nodes_bytes",
    "plan_price_free_leaves_bytes",
    "plan_price_free_levels_bytes",
    "plan_default_order_capacity",
    "plan_price_internal_node_capacity",
    "plan_price_leaf_capacity",
    "plan_price_level_capacity",
)


class ValidationError(RuntimeError):
    """An artifact is incomplete, malformed, tampered, or incomparable."""


@dataclass(frozen=True)
class Distribution:
    sample_count: int
    metrics: Mapping[str, int]


@dataclass(frozen=True)
class ReplayOutput:
    main: Mapping[str, str]
    ready: Mapping[str, str]
    distributions: Mapping[str, Distribution]


@dataclass(frozen=True)
class Artifact:
    label: str
    root: Path
    preflight: Mapping[str, str]
    build: Mapping[str, str]
    source_build: Mapping[str, str]
    provenance: Mapping[str, str]
    summary: Sequence[Mapping[str, str]]
    run_ids: Tuple[str, ...]
    latency: Mapping[str, ReplayOutput]
    correctness: ReplayOutput


def error(context: str, message: str) -> ValidationError:
    return ValidationError(f"{context}: {message}")


def run_ids_for_count(count: int) -> Tuple[str, ...]:
    return tuple(f"{run:03d}" for run in range(1, count + 1))


def read_text(path: Path, context: str) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as exc:
        raise error(context, f"cannot read UTF-8 file {path}: {exc}") from exc


def read_nul_argv(path: Path, context: str) -> Tuple[str, ...]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise error(context, f"cannot read argv file {path}: {exc}") from exc
    if not data or not data.endswith(b"\0"):
        raise error(context, f"argv file is empty or not NUL-terminated: {path}")
    raw_arguments = data[:-1].split(b"\0")
    if any(not argument for argument in raw_arguments):
        raise error(context, f"argv file contains an empty argument: {path}")
    try:
        arguments = tuple(argument.decode("utf-8", errors="strict") for argument in raw_arguments)
    except UnicodeError as exc:
        raise error(context, f"argv file is not UTF-8: {path}") from exc
    if any(any(character in argument for character in "\r\n\0") for argument in arguments):
        raise error(context, f"argv file contains a control character: {path}")
    return arguments


def decode_shell_word(value: str, context: str, field: str) -> str:
    try:
        words = shlex.split(value, posix=True)
    except ValueError as exc:
        raise error(context, f"{field} is not valid shell quoting") from exc
    if len(words) != 1:
        raise error(context, f"{field} does not encode exactly one shell word")
    return words[0]


def validate_shell_command(
    path: Path, expected: Sequence[str], context: str
) -> None:
    command = read_text(path, context)
    if not command.endswith("\n") or "\n" in command[:-1]:
        raise error(context, f"retained command is not exactly one line: {path}")
    try:
        arguments = shlex.split(command, posix=True)
    except ValueError as exc:
        raise error(context, f"retained command has invalid quoting: {path}") from exc
    if tuple(arguments) != tuple(expected):
        raise error(context, f"retained command text differs from argv: {path}")


def cmake_cache_values(
    path: Path, keys: Iterable[str], context: str
) -> Dict[str, str]:
    wanted = set(keys)
    values: Dict[str, str] = {}
    for line_number, line in enumerate(read_text(path, context).splitlines(), 1):
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        typed_key, value = line.split("=", 1)
        key = typed_key.split(":", 1)[0]
        if key not in wanted:
            continue
        if key in values:
            raise error(context, f"duplicate CMake cache key {key} at line {line_number}")
        values[key] = value
    missing = sorted(wanted - values.keys())
    if missing:
        raise error(context, f"fresh CMake cache lacks: {', '.join(missing)}")
    return values


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while True:
                block = source.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    except OSError as exc:
        raise ValidationError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def verify_manifest(root: Path, label: str) -> None:
    context = f"{label} manifest"
    try:
        root_state = root.lstat()
    except OSError as exc:
        raise error(context, f"cannot stat artifact directory {root}: {exc}") from exc
    if not stat.S_ISDIR(root_state.st_mode) or root.is_symlink():
        raise error(context, "artifact root must be a real directory, not a symlink")

    manifest = root / "manifest.sha256"
    try:
        manifest_state = manifest.lstat()
    except OSError as exc:
        raise error(context, f"missing manifest.sha256: {exc}") from exc
    if not stat.S_ISREG(manifest_state.st_mode) or manifest.is_symlink():
        raise error(context, "manifest.sha256 must be a regular non-symlink file")

    try:
        manifest_bytes = manifest.read_bytes()
    except OSError as exc:
        raise error(context, f"cannot read manifest.sha256: {exc}") from exc
    if not manifest_bytes or not manifest_bytes.endswith(b"\n"):
        raise error(context, "manifest must be nonempty and newline terminated")
    try:
        manifest_text = manifest_bytes.decode("ascii", errors="strict")
    except UnicodeError as exc:
        raise error(context, "manifest is not ASCII") from exc

    entries: List[Tuple[str, str]] = []
    seen = set()
    for line_number, line in enumerate(manifest_text.splitlines(), 1):
        match = re.fullmatch(r"([0-9a-f]{64})  (\./[^\r\n]+)", line)
        if match is None:
            raise error(context, f"malformed line {line_number}")
        expected_digest, relative = match.groups()
        if SAFE_MANIFEST_PATH_RE.fullmatch(relative) is None:
            raise error(context, f"unsafe path on line {line_number}: {relative!r}")
        components = relative[2:].split("/")
        if any(component in ("", ".", "..") for component in components):
            raise error(context, f"non-canonical path on line {line_number}: {relative!r}")
        if relative == "./manifest.sha256":
            raise error(context, "manifest must not hash itself")
        if relative in seen:
            raise error(context, f"duplicate entry: {relative}")
        seen.add(relative)
        entries.append((relative, expected_digest))

    manifest_paths = [relative for relative, _ in entries]
    if manifest_paths != sorted(manifest_paths):
        raise error(context, "entries are not in canonical sorted order")

    actual_paths: List[str] = []
    for current, directories, files in os.walk(str(root), topdown=True, followlinks=False):
        current_path = Path(current)
        for directory in list(directories):
            directory_path = current_path / directory
            try:
                state = directory_path.lstat()
            except OSError as exc:
                raise error(context, f"cannot stat {directory_path}: {exc}") from exc
            if not stat.S_ISDIR(state.st_mode) or directory_path.is_symlink():
                raise error(context, f"symlink or non-directory found: {directory_path}")
        for filename in files:
            path = current_path / filename
            try:
                state = path.lstat()
            except OSError as exc:
                raise error(context, f"cannot stat {path}: {exc}") from exc
            if not stat.S_ISREG(state.st_mode) or path.is_symlink():
                raise error(context, f"non-regular file or symlink found: {path}")
            relative = "./" + path.relative_to(root).as_posix()
            if relative != "./manifest.sha256":
                if SAFE_MANIFEST_PATH_RE.fullmatch(relative) is None:
                    raise error(context, f"artifact contains unsafe path: {relative!r}")
                actual_paths.append(relative)

    actual_paths.sort()
    if manifest_paths != actual_paths:
        missing = sorted(set(actual_paths) - set(manifest_paths))
        stale = sorted(set(manifest_paths) - set(actual_paths))
        raise error(
            context,
            f"manifest/file-set mismatch; unlisted={missing!r} missing={stale!r}",
        )

    for relative, expected_digest in entries:
        path = root / relative[2:]
        actual_digest = sha256_file(path)
        if actual_digest != expected_digest:
            raise error(
                context,
                f"SHA-256 mismatch for {relative}: {actual_digest} != {expected_digest}",
            )


def snapshot_artifact(source: Path, destination: Path, label: str) -> None:
    context = f"{label} immutable snapshot"
    source_manifest = source / "manifest.sha256"
    manifest_sha256_before = sha256_file(source_manifest)
    verify_manifest(source, f"{label} source before snapshot")
    manifest_sha256_after_verification = sha256_file(source_manifest)
    if manifest_sha256_after_verification != manifest_sha256_before:
        raise error(context, "artifact manifest changed during source verification")
    try:
        shutil.copytree(source, destination, symlinks=True)
    except OSError as exc:
        raise error(context, f"cannot copy artifact into private snapshot: {exc}") from exc
    manifest_sha256_after = sha256_file(source_manifest)
    snapshot_manifest_sha256 = sha256_file(destination / "manifest.sha256")
    if (
        manifest_sha256_before != manifest_sha256_after
        or snapshot_manifest_sha256 != manifest_sha256_before
    ):
        raise error(context, "artifact manifest changed while the snapshot was copied")
    verify_manifest(destination, f"{label} private snapshot")


def unique_equals_fields(path: Path, keys: Iterable[str], context: str) -> Dict[str, str]:
    wanted = set(keys)
    values: Dict[str, str] = {}
    for line_number, line in enumerate(read_text(path, context).splitlines(), 1):
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key not in wanted:
            continue
        if key in values:
            raise error(context, f"duplicate {key}= field at line {line_number}")
        if not value or any(character in value for character in "\r\n\t\0"):
            raise error(context, f"empty or invalid value for {key}")
        values[key] = value
    missing = sorted(wanted - values.keys())
    if missing:
        raise error(context, f"missing required fields: {', '.join(missing)}")
    return values


def uint(value: str, context: str, field: str) -> int:
    if UINT_RE.fullmatch(value) is None:
        raise error(context, f"{field} is not a canonical unsigned integer: {value!r}")
    return int(value)


def positive_uint(value: str, context: str, field: str) -> int:
    parsed = uint(value, context, field)
    if parsed == 0:
        raise error(context, f"{field} must be positive")
    return parsed


def parse_capacity_manifest(path: Path, context: str) -> Mapping[str, str]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise error(context, f"cannot read capacity evidence manifest: {exc}") from exc
    if not data or len(data) > 16 * 1024:
        raise error(context, "capacity evidence manifest size is outside [1, 16384]")
    if not data.endswith(b"\n") or b"\r" in data or b"\0" in data:
        raise error(context, "capacity evidence manifest is not canonical LF text")
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as exc:
        raise error(context, "capacity evidence manifest is not ASCII") from exc

    expected_keys = (
        "schema",
        "profile_name",
        "corpus_manifest_sha256",
        "profiler_sha256",
        "order_direct_slots",
        "order_fallback_buckets",
        "price_page_capacity",
        "profiled_max_order_ref",
        "profiled_unique_price_pages",
        "minimum_direct_order_headroom",
        "minimum_price_page_headroom",
    )
    lines = text[:-1].split("\n")
    if len(lines) != len(expected_keys):
        raise error(context, "capacity evidence manifest line count is not canonical")
    fields: Dict[str, str] = {}
    for line, expected_key in zip(lines, expected_keys):
        if line.count("=") != 1:
            raise error(context, "capacity evidence manifest contains a malformed line")
        key, value = line.split("=", 1)
        if key != expected_key or not value:
            raise error(context, "capacity evidence manifest keys/order differ from schema")
        fields[key] = value

    if fields["schema"] != CAPACITY_MANIFEST_SCHEMA:
        raise error(context, "capacity evidence manifest schema is unsupported")
    if CAPACITY_PROFILE_RE.fullmatch(fields["profile_name"]) is None:
        raise error(context, "capacity evidence manifest profile name is invalid")
    for field in ("corpus_manifest_sha256", "profiler_sha256"):
        if HEX64_RE.fullmatch(fields[field]) is None:
            raise error(context, f"capacity evidence manifest {field} is not SHA-256")
    numeric_fields = expected_keys[4:]
    numbers = {
        field: positive_uint(fields[field], context, field)
        for field in numeric_fields
    }
    for field in (
        "order_direct_slots",
        "profiled_max_order_ref",
        "minimum_direct_order_headroom",
    ):
        if numbers[field] > 0xFFFFFFFFFFFFFFFF:
            raise error(context, f"capacity evidence {field} exceeds uint64")
    if numbers["order_fallback_buckets"] > 0xFFFFFFFF or (
        numbers["order_fallback_buckets"]
        & (numbers["order_fallback_buckets"] - 1)
    ):
        raise error(context, "capacity evidence fallback buckets are not uint32 power-of-two")
    if numbers["price_page_capacity"] >= 0xFFFFFFFF:
        raise error(context, "capacity evidence price-page capacity is outside uint32")
    if numbers["profiled_unique_price_pages"] >= 0xFFFFFFFF:
        raise error(context, "capacity evidence profiled page demand is outside uint32")
    if numbers["minimum_price_page_headroom"] > 0xFFFFFFFF:
        raise error(context, "capacity evidence minimum page headroom exceeds uint32")
    direct_headroom = (
        numbers["order_direct_slots"]
        - (numbers["profiled_max_order_ref"] + 1)
    )
    if direct_headroom < numbers["minimum_direct_order_headroom"]:
        raise error(context, "capacity evidence direct-order headroom is insufficient")
    page_headroom = (
        numbers["price_page_capacity"]
        - numbers["profiled_unique_price_pages"]
    )
    if page_headroom < numbers["minimum_price_page_headroom"]:
        raise error(context, "capacity evidence price-page headroom is insufficient")
    return {**fields, "manifest_sha256": hashlib.sha256(data).hexdigest()}


def validate_capacity_capture_state(
    path: Path,
    expected_sha256: str,
    context: str,
) -> Tuple[str, str]:
    fields = unique_equals_fields(
        path,
        (
            "path",
            "sha256",
            "stat_before_hash",
            "stat_after_hash",
            "stable_during_capture",
        ),
        context,
    )
    captured_path = decode_shell_word(fields["path"], context, "path")
    if not captured_path.startswith("/"):
        raise error(context, "captured capacity path is not absolute")
    if fields["sha256"] != expected_sha256:
        raise error(context, "captured capacity SHA-256 differs from profile identity")
    if fields["stable_during_capture"] != "1":
        raise error(context, "capacity evidence was unstable during capture")
    if fields["stat_before_hash"] != fields["stat_after_hash"]:
        raise error(context, "capacity evidence changed while it was hashed")
    return captured_path, fields["stat_after_hash"]


def parse_cpu_list(value: str, context: str, field: str) -> Set[int]:
    if value in ("none", "unavailable", ""):
        return set()
    result: Set[int] = set()
    for item in value.split(","):
        if re.fullmatch(r"[0-9]+", item):
            first = last = int(item)
        else:
            match = re.fullmatch(r"([0-9]+)-([0-9]+)", item)
            if match is None:
                raise error(context, f"{field} has invalid CPU-list syntax: {value!r}")
            first, last = (int(part) for part in match.groups())
            if last < first:
                raise error(context, f"{field} has descending range: {item!r}")
        result.update(range(first, last + 1))
    return result


def parse_record(line: str, context: str) -> Tuple[str, Dict[str, str]]:
    tokens = line.split()
    if not tokens:
        raise error(context, "empty record")
    fields: Dict[str, str] = {}
    for token in tokens[1:]:
        if token.count("=") != 1:
            raise error(context, f"malformed record token: {token!r}")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise error(context, f"empty or duplicate record field: {token!r}")
        fields[key] = value
    return tokens[0], fields


def one_record(lines: Sequence[str], name: str, context: str) -> Dict[str, str]:
    matching = [line for line in lines if line.split(None, 1)[0] == name]
    if len(matching) != 1:
        raise error(context, f"expected exactly one {name} record, found {len(matching)}")
    record_name, fields = parse_record(matching[0], context)
    if record_name != name:
        raise error(context, f"record name changed while parsing {name}")
    return fields


def require_fields(fields: Mapping[str, str], names: Iterable[str], context: str) -> None:
    missing = sorted(set(names) - fields.keys())
    if missing:
        raise error(context, f"record lacks required fields: {', '.join(missing)}")


def validate_distribution(
    fields: Mapping[str, str], context: str, require_samples: bool = True
) -> Distribution:
    require_fields(fields, ("sample_count",) + METRICS, context)
    samples = uint(fields["sample_count"], context, "sample_count")
    if require_samples and samples == 0:
        raise error(context, "sample_count must be nonzero")
    metrics = {metric: uint(fields[metric], context, metric) for metric in METRICS}
    values = [metrics[metric] for metric in METRICS]
    if values != sorted(values):
        raise error(context, "latency percentiles/max are not monotonic")
    return Distribution(samples, metrics)


BASE_MAIN_NUMERIC_FIELDS: Tuple[str, ...] = (
    "records",
    "bytes",
    "prelude_records",
    "prelude_bytes",
    "book_messages",
    "applied_book_mutations",
    "sample_count",
    "sample_every",
    "warmup_book_messages",
    "min_samples",
    "sample_capacity",
    "sample_storage_prefaulted",
    "post_warmup_minor_faults",
    "post_warmup_major_faults",
    "prefault",
    "storage_system_page_bytes",
    "rdtsc_overhead_ticks",
    "rdtsc_ticks_per_second",
    "now_ns_overhead_ns",
    "price_capacity_failures",
    "final_live_orders",
    "phase",
    "mutation_digest_enabled",
    "semantic_mutation_digest_enabled",
) + METRICS

REDESIGN_MAIN_NUMERIC_FIELDS: Tuple[str, ...] = (
    "direct_order_slots",
    "fallback_buckets",
    "price_page_capacity",
    "effective_mapped_bytes",
    "effective_direct_orders_mapped_bytes",
    "effective_price_pages_mapped_bytes",
    "effective_descriptor_bytes",
    "effective_storage_bytes",
    "price_pages",
)

BRANCH5_MAIN_NUMERIC_FIELDS: Tuple[str, ...] = (
    "native_prefault_complete",
    "book_universe_sealed",
    "prepared_books",
    "native_range_count",
    "native_range_bytes",
    "native_range_digest",
    "price_internal_node_exhaustions",
    "price_leaf_exhaustions",
    "price_level_exhaustions",
)

BASE_READY_NUMERIC_FIELDS: Tuple[str, ...] = (
    "prefault",
    "prelude_records",
    "prelude_bytes",
    "sample_capacity",
    "sample_storage_prefaulted",
    "start_gate_enabled",
    "sample_every",
    "warmup_book_messages",
)

REDESIGN_READY_NUMERIC_FIELDS: Tuple[str, ...] = (
    "effective_storage_bytes",
    "direct_orders_base",
    "direct_orders_mapped_bytes",
    "price_pages_base",
    "price_pages_mapped_bytes",
)

BRANCH5_READY_NUMERIC_FIELDS: Tuple[str, ...] = (
    "native_prefault_complete",
    "book_universe_sealed",
    "prepared_books",
    "native_range_count",
    "native_range_bytes",
    "native_range_digest",
)


def parse_replay_output(
    path: Path,
    context: str,
    preflight: Mapping[str, str],
    expected_capacity: Mapping[str, str],
    correctness: bool,
) -> ReplayOutput:
    lines = [line for line in read_text(path, context).splitlines() if line.strip()]
    main = one_record(lines, "itch_book_replay", context)
    ready = one_record(lines, "itch_book_replay_ready", context)
    require_fields(main, ("hot_arena_schema",), context)
    require_fields(ready, ("hot_arena_schema",), context)
    schema = main["hot_arena_schema"]
    if schema not in (REDESIGN_SCHEMA, BRANCH5_NATIVE_SCHEMA):
        raise error(context, f"unsupported hot-arena schema: {schema!r}")
    if ready["hot_arena_schema"] != schema:
        raise error(context, "ready/final hot-arena schemas differ")
    if preflight["plan_hot_arena_schema"] != schema:
        raise error(context, "replay hot-arena schema differs from preflight")
    expected_policy = (
        "redesign_exact_v1"
        if schema == REDESIGN_SCHEMA
        else "branch5_native_ranges_v1"
    )
    if preflight["plan_hot_arena_policy"] != expected_policy:
        raise error(context, "preflight hot-arena policy does not match schema")

    schema_main_fields = (
        REDESIGN_MAIN_NUMERIC_FIELDS
        if schema == REDESIGN_SCHEMA
        else BRANCH5_MAIN_NUMERIC_FIELDS
    )
    schema_ready_fields = (
        REDESIGN_READY_NUMERIC_FIELDS
        if schema == REDESIGN_SCHEMA
        else BRANCH5_READY_NUMERIC_FIELDS
    )
    main_numeric_fields = BASE_MAIN_NUMERIC_FIELDS + schema_main_fields
    ready_numeric_fields = BASE_READY_NUMERIC_FIELDS + schema_ready_fields
    require_fields(
        main,
        main_numeric_fields + ("sample_strategy", "sample_schedule_id"),
        context,
    )
    require_fields(ready, ready_numeric_fields + ("sample_schedule_id",), context)

    main_numbers = {
        field: uint(main[field], context, field) for field in main_numeric_fields
    }
    ready_numbers = {
        field: uint(ready[field], context, f"ready.{field}")
        for field in ready_numeric_fields
    }

    expected_records = uint(preflight["expected_records"], context, "expected_records")
    expected_bytes = uint(preflight["expected_bytes"], context, "expected_bytes")
    sample_every = uint(preflight["sample_every"], context, "preflight.sample_every")
    warmup = uint(
        preflight["warmup_book_messages"], context, "preflight.warmup_book_messages"
    )
    minimum_samples = uint(preflight["min_samples"], context, "preflight.min_samples")

    if main_numbers["records"] != expected_records:
        raise error(context, "record count differs from preflight")
    if main_numbers["bytes"] != expected_bytes:
        raise error(context, "byte count differs from preflight")
    for field, total in (("prelude_records", "records"), ("prelude_bytes", "bytes")):
        if main_numbers[field] == 0 or main_numbers[field] > main_numbers[total]:
            raise error(context, f"{field} is outside the replay extent")
        if ready_numbers[field] != main_numbers[field]:
            raise error(context, f"ready/final {field} differs")
    if main_numbers["book_messages"] == 0:
        raise error(context, "book_messages must be nonzero")
    if main_numbers["applied_book_mutations"] != main_numbers["book_messages"]:
        raise error(context, "not every book message was applied")
    if main_numbers["sample_count"] < minimum_samples:
        raise error(context, "sample_count is below min_samples")
    if main_numbers["sample_capacity"] < main_numbers["sample_count"]:
        raise error(context, "sample capacity is below sample count")
    if main_numbers["sample_every"] != sample_every or ready_numbers["sample_every"] != sample_every:
        raise error(context, "sample_every differs between preflight/ready/final")
    if main_numbers["warmup_book_messages"] != warmup or ready_numbers["warmup_book_messages"] != warmup:
        raise error(context, "warmup differs between preflight/ready/final")
    if main_numbers["min_samples"] != minimum_samples:
        raise error(context, "min_samples differs from preflight")
    if main["sample_strategy"] != "fixed_seed_block_offset":
        raise error(context, "unknown sample strategy")
    if main["sample_schedule_id"] != SAMPLE_SCHEDULE_ID or ready["sample_schedule_id"] != SAMPLE_SCHEDULE_ID:
        raise error(context, "unknown or inconsistent sample schedule ID")

    if main_numbers["prefault"] != 1 or ready_numbers["prefault"] != 1:
        raise error(context, "storage was not prefaulted")
    if main_numbers["sample_storage_prefaulted"] != 1 or ready_numbers["sample_storage_prefaulted"] != 1:
        raise error(context, "sample storage was not prefaulted")
    if main_numbers["post_warmup_minor_faults"] != 0 or main_numbers["post_warmup_major_faults"] != 0:
        raise error(context, "post-warmup page faults are nonzero")
    if ready_numbers["start_gate_enabled"] != 1:
        raise error(context, "post-prefault start gate was not enabled")
    if ready_numbers["sample_capacity"] != main_numbers["sample_capacity"]:
        raise error(context, "ready/final sample capacities differ")

    for nonzero in (
        "sample_capacity",
        "storage_system_page_bytes",
        "rdtsc_ticks_per_second",
    ):
        if main_numbers[nonzero] == 0:
            raise error(context, f"{nonzero} must be nonzero")
    if main_numbers["price_capacity_failures"] != 0:
        raise error(context, "price capacity failures are nonzero")
    if main_numbers["final_live_orders"] != 0 or main_numbers["phase"] != 7:
        raise error(context, "replay did not finish at End of Messages with zero live orders")

    if schema == REDESIGN_SCHEMA:
        require_fields(main, CAPACITY_REPLAY_FIELDS, context)
        require_fields(ready, CAPACITY_REPLAY_FIELDS, context)
        for field in CAPACITY_REPLAY_FIELDS:
            expected = preflight[field]
            if main[field] != expected or ready[field] != expected:
                raise error(
                    context,
                    f"plan/ready/final capacity identity differs for {field}",
                )
        if ready_numbers["effective_storage_bytes"] != main_numbers["effective_storage_bytes"]:
            raise error(context, "ready/final storage sizes differ")
        if ready_numbers["direct_orders_mapped_bytes"] != main_numbers["effective_direct_orders_mapped_bytes"]:
            raise error(context, "ready/final direct-order VMA sizes differ")
        if ready_numbers["price_pages_mapped_bytes"] != main_numbers["effective_price_pages_mapped_bytes"]:
            raise error(context, "ready/final price-page VMA sizes differ")
        for nonzero in (
            "direct_order_slots",
            "fallback_buckets",
            "price_page_capacity",
            "effective_mapped_bytes",
            "effective_direct_orders_mapped_bytes",
            "effective_price_pages_mapped_bytes",
            "effective_storage_bytes",
            "price_pages",
        ):
            if main_numbers[nonzero] == 0:
                raise error(context, f"{nonzero} must be nonzero")
        for field in (
            "direct_order_slots",
            "fallback_buckets",
            "price_page_capacity",
        ):
            if main[field] != expected_capacity[field]:
                raise error(
                    context,
                    f"final {field} differs from authenticated storage plan",
                )
        if ready_numbers["direct_orders_base"] == 0 or ready_numbers["price_pages_base"] == 0:
            raise error(context, "ready VMA base is zero")
        if main_numbers["price_pages"] > main_numbers["price_page_capacity"]:
            raise error(context, "committed price pages exceed capacity")
    else:
        require_fields(
            main,
            ("native_range_manifest_schema", "native_range_manifest"),
            context,
        )
        require_fields(
            ready,
            ("native_range_manifest_schema", "native_range_manifest"),
            context,
        )
        if (
            main["native_range_manifest_schema"]
            != BRANCH5_NATIVE_MANIFEST_SCHEMA
            or ready["native_range_manifest_schema"]
            != BRANCH5_NATIVE_MANIFEST_SCHEMA
        ):
            raise error(context, "unknown branch5 native-range manifest schema")
        if main["native_range_manifest"] != ready["native_range_manifest"]:
            raise error(context, "branch5 ready/final native manifest paths differ")
        if not re.fullmatch(r'"[^"\r\n]+"', main["native_range_manifest"]):
            raise error(context, "branch5 native manifest path is not quoted")
        for flag in ("native_prefault_complete", "book_universe_sealed"):
            if main_numbers[flag] != 1 or ready_numbers[flag] != 1:
                raise error(context, f"branch5 {flag} is not persistently complete")
        for field in (
            "prepared_books",
            "native_range_count",
            "native_range_bytes",
            "native_range_digest",
        ):
            if main_numbers[field] != ready_numbers[field]:
                raise error(context, f"branch5 ready/final {field} differs")
        if main_numbers["prepared_books"] == 0 or main_numbers["native_range_bytes"] == 0:
            raise error(context, "branch5 native range totals are empty")
        if main_numbers["native_range_count"] != 6 + 4 * main_numbers["prepared_books"]:
            raise error(context, "branch5 native range count differs from prepared books")
        planned_bytes = positive_uint(
            preflight["planned_bytes"], context, "preflight.planned_bytes"
        )
        if main_numbers["native_range_bytes"] > planned_bytes:
            raise error(
                context,
                "branch5 native range payload exceeds the admitted planned bytes",
            )
        if preflight["trace_sha256"] == BRANCH5_PINNED_TRACE_SHA256 and (
            main_numbers["prepared_books"] != BRANCH5_PINNED_PREPARED_BOOKS
            or main_numbers["native_range_count"]
            != BRANCH5_PINNED_NATIVE_RANGE_COUNT
            or main_numbers["native_range_bytes"]
            != BRANCH5_PINNED_NATIVE_RANGE_BYTES
        ):
            raise error(
                context,
                "branch5 native totals differ from the pinned full-trace profile",
            )
        if (
            preflight["trace_sha256"] == BRANCH5_PINNED_TRACE_SHA256
            and main_numbers["sample_capacity"]
            != BRANCH5_PINNED_SAMPLE_CAPACITY
        ):
            raise error(
                context,
                "branch5 effective sample capacity differs from the pinned profile",
            )
        for field in (
            "price_internal_node_exhaustions",
            "price_leaf_exhaustions",
            "price_level_exhaustions",
        ):
            if main_numbers[field] != 0:
                raise error(context, f"branch5 {field} is nonzero")

    digest_fields = {
        "mutation_digest",
        "semantic_mutation_digest",
        "semantic_mutation_digest_schema",
    }
    if correctness:
        if main_numbers["mutation_digest_enabled"] != 1 or main_numbers["semantic_mutation_digest_enabled"] != 1:
            raise error(context, "correctness digest modes are not both enabled")
        require_fields(main, digest_fields, context)
        uint(main["mutation_digest"], context, "mutation_digest")
        uint(main["semantic_mutation_digest"], context, "semantic_mutation_digest")
        if main["semantic_mutation_digest_schema"] != SEMANTIC_DIGEST_SCHEMA:
            raise error(context, "unknown semantic mutation digest schema")
    else:
        if main_numbers["mutation_digest_enabled"] != 0 or main_numbers["semantic_mutation_digest_enabled"] != 0:
            raise error(context, "latency run unexpectedly enabled correctness digests")
        unexpected = sorted(digest_fields.intersection(main.keys()))
        if unexpected:
            raise error(context, f"latency output contains digest fields: {unexpected!r}")

    aggregate = validate_distribution(main, f"{context} aggregate")
    type_lines = [line for line in lines if line.split(None, 1)[0] == "itch_book_replay_type"]
    if len(type_lines) != len(MESSAGE_TYPES):
        raise error(context, f"expected seven type distributions, found {len(type_lines)}")
    distributions: Dict[str, Distribution] = {"aggregate": aggregate}
    for line in type_lines:
        _, fields = parse_record(line, context)
        require_fields(fields, ("type", "sample_count") + METRICS, context)
        message_type = fields["type"]
        if message_type not in MESSAGE_TYPES or message_type in distributions:
            raise error(context, f"unknown or duplicate type distribution: {message_type!r}")
        distributions[message_type] = validate_distribution(
            fields, f"{context} type {message_type}"
        )
    if set(distributions) != set(WORKLOADS):
        raise error(context, "type distribution set is incomplete")
    if sum(distributions[item].sample_count for item in MESSAGE_TYPES) != aggregate.sample_count:
        raise error(context, "type sample counts do not sum to aggregate sample count")
    if max(distributions[item].metrics["max_ns"] for item in MESSAGE_TYPES) != aggregate.metrics["max_ns"]:
        raise error(context, "aggregate max does not equal maximum type max")
    return ReplayOutput(main=main, ready=ready, distributions=distributions)


def parse_summary(path: Path, context: str) -> List[Dict[str, str]]:
    text = read_text(path, context)
    if not text.endswith("\n"):
        raise error(context, "summary is not newline terminated")
    rows = list(csv.reader(text.splitlines(), delimiter="\t", strict=True))
    if not rows or tuple(rows[0]) != SUMMARY_HEADER:
        raise error(context, "summary header/schema is not exact")
    result: List[Dict[str, str]] = []
    seen = set()
    allowed_kinds = {
        "latency",
        "correctness",
        "hardware-counters",
        "provenance",
        "worst",
        "overall",
    }
    for line_number, row in enumerate(rows[1:], 2):
        if len(row) != len(SUMMARY_HEADER):
            raise error(context, f"line {line_number} has {len(row)} columns")
        record = dict(zip(SUMMARY_HEADER, row))
        identity = (record["kind"], record["id"])
        if identity in seen:
            raise error(context, f"duplicate summary row {identity!r}")
        seen.add(identity)
        if record["kind"] not in allowed_kinds:
            raise error(context, f"unknown summary kind {record['kind']!r}")
        if any("\0" in value or "\r" in value or "\n" in value for value in row):
            raise error(context, f"control character in line {line_number}")
        result.append(record)
    return result


def exactly_one_summary(
    rows: Sequence[Mapping[str, str]], kind: str, identity: str, context: str
) -> Mapping[str, str]:
    matches = [row for row in rows if row["kind"] == kind and row["id"] == identity]
    if len(matches) != 1:
        raise error(context, f"expected one summary row ({kind}, {identity}), found {len(matches)}")
    return matches[0]


def require_regular_files(root: Path, names: Iterable[str], context: str) -> None:
    for name in names:
        path = root / name
        try:
            state = path.lstat()
        except OSError as exc:
            raise error(context, f"required evidence is missing: {name}: {exc}") from exc
        if not stat.S_ISREG(state.st_mode) or path.is_symlink():
            raise error(context, f"required evidence is not a regular file: {name}")


def branch5_order_range_sizes(capacity: int) -> Tuple[int, int, int, int]:
    return (
        32 * capacity,
        4 * capacity,
        8 * ((capacity + 63) // 64),
        64 * capacity,
    )


def branch5_pinned_order_capacity(locate: int) -> int:
    if locate in BRANCH5_ACTIVE_LOCATES:
        return 262144
    if locate in BRANCH5_HOT_LOCATES:
        return 1048576
    if locate in BRANCH5_ULTRA_HOT_LOCATES:
        return 4194304
    return 65536


def validate_branch5_native_manifest(
    path: Path,
    ready: Mapping[str, str],
    context: str,
    *,
    pinned_trace: bool = False,
) -> None:
    require_regular_files(path.parent, (path.name,), context)
    data = path.read_bytes()
    if not data or not data.endswith(b"\n") or b"\r" in data:
        raise error(context, "branch5 native manifest is not LF-terminated")
    try:
        lines = data.decode("ascii", errors="strict").splitlines()
    except UnicodeError as exc:
        raise error(context, "branch5 native manifest is not ASCII") from exc
    header = re.fullmatch(
        r"branch5_native_ranges schema=branch5_native_ranges_v1 "
        r"count=([0-9]+) bytes=([0-9]+) digest=([0-9]+)",
        lines[0],
    )
    if header is None:
        raise error(context, "branch5 native manifest header is malformed")
    row_pattern = re.compile(
        r"branch5_native_range ordinal=([0-9]+) kind=([a-z_]+) "
        r"locate=([0-9]+) base=([0-9]+) bytes=([0-9]+)"
    )
    rows: List[Tuple[str, int, int, int]] = []
    for ordinal, line in enumerate(lines[1:]):
        match = row_pattern.fullmatch(line)
        if match is None:
            raise error(context, f"branch5 native manifest row {ordinal} is malformed")
        ordinal_text, kind, locate_text, base_text, size_text = match.groups()
        if int(ordinal_text) != ordinal:
            raise error(context, "branch5 native manifest ordinals are not contiguous")
        locate = int(locate_text)
        base = int(base_text)
        size = int(size_text)
        if (
            locate < 0
            or locate > 65535
            or base <= 0
            or size <= 0
            or base + size > (1 << 64) - 1
        ):
            raise error(context, "branch5 native manifest has invalid span")
        rows.append((kind, locate, base, size))

    header_count, header_bytes, header_digest = map(int, header.groups())
    expected_count = uint(ready["native_range_count"], context, "native_range_count")
    expected_bytes = uint(ready["native_range_bytes"], context, "native_range_bytes")
    expected_digest = uint(ready["native_range_digest"], context, "native_range_digest")
    prepared_books = uint(ready["prepared_books"], context, "prepared_books")
    if header_count != len(rows) or header_count != expected_count:
        raise error(context, "branch5 native manifest count differs from ready marker")
    if header_count != 6 + 4 * prepared_books:
        raise error(context, "branch5 native manifest count differs from prepared books")

    global_kinds = (
        "price_nodes",
        "price_leaves",
        "price_levels",
        "price_free_nodes",
        "price_free_leaves",
        "price_free_levels",
    )
    book_kinds = (
        "order_records",
        "order_free_indices",
        "order_occupancy",
        "order_ref_entries",
    )
    if tuple((kind, locate) for kind, locate, _, _ in rows[:6]) != tuple(
        (kind, 0) for kind in global_kinds
    ):
        raise error(context, "branch5 native global range order is invalid")
    expected_global_sizes = tuple(
        int(BRANCH5_PINNED_PRICE_VECTOR_FIELDS[f"{kind}_bytes"])
        for kind in global_kinds
    )
    if tuple(row[3] for row in rows[:6]) != expected_global_sizes:
        raise error(
            context,
            "branch5 native global ranges differ from the pinned price-pool layout",
        )
    prepared_locates: List[int] = []
    order_capacity_counts: Dict[int, int] = {}
    for start in range(6, len(rows), 4):
        group = rows[start : start + 4]
        if len(group) != 4 or tuple(row[0] for row in group) != book_kinds:
            raise error(context, "branch5 native per-book range group is invalid")
        locates = {row[1] for row in group}
        if len(locates) != 1 or next(iter(locates)) == 0:
            raise error(context, "branch5 native per-book locate group is invalid")
        locate = next(iter(locates))
        prepared_locates.append(locate)
        order_sizes = tuple(row[3] for row in group)
        if order_sizes[0] % 32 != 0:
            raise error(context, "branch5 order-record range is not capacity aligned")
        order_capacity = order_sizes[0] // 32
        if (
            order_capacity not in BRANCH5_ORDER_CAPACITIES
            or order_sizes != branch5_order_range_sizes(order_capacity)
        ):
            raise error(
                context,
                "branch5 per-book ranges differ from a canonical order tier",
            )
        order_capacity_counts[order_capacity] = (
            order_capacity_counts.get(order_capacity, 0) + 1
        )
        if pinned_trace and order_capacity != branch5_pinned_order_capacity(locate):
            raise error(
                context,
                "branch5 per-book range tier differs from the pinned trace directory",
            )
    if any(left >= right for left, right in zip(prepared_locates, prepared_locates[1:])):
        raise error(context, "branch5 native prepared locates are not ascending")
    if pinned_trace:
        if (
            prepared_books != BRANCH5_PINNED_PREPARED_BOOKS
            or header_count != BRANCH5_PINNED_NATIVE_RANGE_COUNT
            or expected_bytes != BRANCH5_PINNED_NATIVE_RANGE_BYTES
            or prepared_locates
            != list(range(1, BRANCH5_PINNED_PREPARED_BOOKS + 1))
            or order_capacity_counts
            != {
                65536: 8696,
                262144: 6,
                1048576: 6,
                4194304: 5,
            }
        ):
            raise error(
                context,
                "branch5 native manifest differs from the pinned full-trace book universe",
            )

    total_bytes = sum(row[3] for row in rows)
    if total_bytes != header_bytes or total_bytes != expected_bytes:
        raise error(context, "branch5 native manifest byte total differs")
    spans = sorted((base, base + size) for _, _, base, size in rows)
    if any(left[1] > right[0] for left, right in zip(spans, spans[1:])):
        raise error(context, "branch5 native manifest spans overlap")

    digest = 14695981039346656037
    for byte in BRANCH5_NATIVE_MANIFEST_SCHEMA.encode("ascii"):
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    for kind, locate, base, size in rows:
        encoded = kind.encode("ascii")
        payload = struct.pack("<Q", len(encoded)) + encoded + struct.pack(
            "<QQQ", locate, base, size
        )
        for byte in payload:
            digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    if digest != header_digest or digest != expected_digest:
        raise error(context, "branch5 native manifest digest differs")


def validate_memory_evidence(
    root: Path,
    prefix: str,
    context: str,
    schema: str,
    *,
    pinned_branch5_trace: bool = False,
) -> None:
    names = [
        f"{prefix}.command.txt",
        f"{prefix}.stdout.log",
        f"{prefix}.stderr.log",
        f"{prefix}.memory-evidence.txt",
        f"{prefix}.smaps_rollup.txt",
        f"{prefix}.smaps.txt",
        f"{prefix}.numa_maps.txt",
        f"{prefix}.numa-summary.txt",
        f"{prefix}.start-gate",
    ]
    if schema == BRANCH5_NATIVE_SCHEMA:
        names.append(f"{prefix}.native-ranges.txt")
    require_regular_files(root, names, context)
    evidence = read_text(root / f"{prefix}.memory-evidence.txt", context)
    marker = "post_prefault_snapshot=validated ready_marker=observed start_gate=released"
    if evidence.splitlines().count(marker) != 1:
        raise error(context, f"{prefix} lacks one validated post-prefault snapshot marker")
    smaps_rollup = read_text(root / f"{prefix}.smaps_rollup.txt", context)
    swap = re.findall(r"^Swap:\s+([0-9]+)\s+kB\s*$", smaps_rollup, flags=re.MULTILINE)
    if swap != ["0"]:
        raise error(context, f"{prefix} smaps_rollup does not prove zero swapped pages")
    numa = unique_equals_fields(
        root / f"{prefix}.numa-summary.txt",
        ("anon_total_pages", "anon_selected_pages", "anon_other_pages"),
        context,
    )
    total = uint(numa["anon_total_pages"], context, "anon_total_pages")
    selected = uint(numa["anon_selected_pages"], context, "anon_selected_pages")
    other = uint(numa["anon_other_pages"], context, "anon_other_pages")
    if total == 0 or selected + other != total or selected * 100 < total * 99:
        raise error(context, f"{prefix} NUMA summary does not prove >=99% selected-node placement")
    if schema == BRANCH5_NATIVE_SCHEMA:
        lines = [
            line
            for line in read_text(root / f"{prefix}.stdout.log", context).splitlines()
            if line.strip()
        ]
        ready = one_record(lines, "itch_book_replay_ready", context)
        require_fields(
            ready,
            (
                "native_range_manifest_schema",
                "prepared_books",
                "native_range_count",
                "native_range_bytes",
                "native_range_digest",
            ),
            context,
        )
        if ready["native_range_manifest_schema"] != BRANCH5_NATIVE_MANIFEST_SCHEMA:
            raise error(context, "branch5 native manifest schema is invalid")
        validate_branch5_native_manifest(
            root / f"{prefix}.native-ranges.txt",
            ready,
            context,
            pinned_trace=pinned_branch5_trace,
        )


PREFLIGHT_FIELDS: Tuple[str, ...] = (
    "artifact_schema",
    "host",
    "kernel",
    "boot_id",
    "machine",
    "cpu_vendor_id",
    "cpu_family",
    "cpu_model",
    "cpu_stepping",
    "cpu_model_name",
    "cpu_online_list",
    "cpu_isolated_list",
    "cpu_isolation_source",
    "cpu_scaling_governor",
    "cpu_scaling_driver",
    "binary_sha256",
    "binary_archive_sha256",
    "source_build_attestation_version",
    "source_build_mode",
    "source_build_target",
    "source_build_attestation_sha256",
    "source_tree_archive_sha256",
    "fresh_binary_sha256",
    "fresh_binary_archive_sha256",
    "trace_size_bytes",
    "trace_sha256",
    "trace_hash_policy",
    *CAPACITY_PREFLIGHT_FIELDS,
    "harness_source_sha256",
    "harness_archive_sha256",
    "hot_path_verifier_source_sha256",
    "hot_path_verifier_archive_sha256",
    "git_dirty",
    "git_commit_before",
    "git_parent_before",
    "git_tree_before",
    "git_branch_before",
    "git_fingerprint_before",
    "cmake_build_type",
    "cmake_generator",
    "cmake_cxx_compiler",
    "cmake_cxx_compiler_id",
    "cmake_cxx_compiler_version",
    "runs",
    "cpu",
    "monitor_cpu",
    "numa_node",
    "node_cpulist",
    "node_mem_total_bytes",
    "expected_records",
    "expected_bytes",
    "sample_every",
    "warmup_book_messages",
    "min_samples",
    "plan_hot_arena_schema",
    "plan_hot_arena_policy",
    "allow_swap",
    "run_perf_stat",
    "perf_version",
    "swap_total_kb",
    "vm_overcommit_memory",
    "vm_overcommit_ratio",
    "thp_enabled",
    "thp_defrag",
    "cgroup_memory_version",
    "cgroup_memory_dir",
    "cgroup_finite_limit_count",
    "cgroup_effective_max",
)

BUILD_FIELDS: Tuple[str, ...] = (
    "cmake_build_type",
    "cmake_generator",
    "cmake_make_program",
    "cmake_cxx_compiler",
    "cmake_cxx_compiler_id",
    "cmake_cxx_compiler_version",
    "cmake_cxx_flags",
    "cmake_cxx_flags_release",
    "cmake_exe_linker_flags",
    "cmake_exe_linker_flags_release",
    "cmake_static_linker_flags",
    "cmake_static_linker_flags_release",
    "source_build_attestation_version",
    "source_build_mode",
    "source_build_environment_policy",
    "source_build_target",
)

SOURCE_BUILD_FIELDS: Tuple[str, ...] = (
    "attestation_version",
    "mode",
    "environment_policy",
    "target",
    "source_commit",
    "source_parent",
    "source_tree",
    "source_branch",
    "source_fingerprint_before",
    "source_fingerprint_after",
    "source_date_epoch",
    "git_command",
    "tar_command",
    "cmake_command",
    "env_command",
    "source_root",
    "source_archive_output",
    "temporary_root",
    "temporary_source",
    "temporary_build",
    "source_archive_sha256",
    "source_archive_argv_sha256",
    "source_extract_argv_sha256",
    "configure_argv_sha256",
    "target_build_argv_sha256",
    "fresh_cmake_cache_sha256",
    "supplied_binary_sha256",
    "fresh_binary_sha256",
    "fresh_binary_archive_sha256",
    "configure_exit_code",
    "target_build_exit_code",
    "result",
    "error",
)

PROVENANCE_FIELDS: Tuple[str, ...] = (
    "provenance_version",
    "binary_sha256_before",
    "binary_sha256_after",
    "binary_archive_sha256_before",
    "binary_archive_sha256_after",
    "trace_sha256_before",
    "trace_sha256_after",
    "capacity_evidence_sha256_before",
    "capacity_evidence_sha256_after",
    "capacity_evidence_archive_sha256_before",
    "capacity_evidence_archive_sha256_after",
    "harness_sha256_before",
    "harness_sha256_after",
    "harness_archive_sha256_before",
    "harness_archive_sha256_after",
    "hot_path_verifier_sha256_before",
    "hot_path_verifier_sha256_after",
    "cmake_cache_sha256_before",
    "cmake_cache_sha256_after",
    "source_build_attestation_sha256_before",
    "source_build_attestation_sha256_after",
    "source_tree_archive_sha256_before",
    "source_tree_archive_sha256_after",
    "fresh_binary_archive_sha256_before",
    "fresh_binary_archive_sha256_after",
    "fresh_cmake_cache_sha256_before",
    "fresh_cmake_cache_sha256_after",
    "git_commit_before",
    "git_commit_after",
    "git_parent_before",
    "git_parent_after",
    "git_tree_before",
    "git_tree_after",
    "git_branch_before",
    "git_branch_after",
    "git_fingerprint_before",
    "git_fingerprint_after",
    "result",
    "error",
)


def validate_source_build_attestation(
    root: Path,
    label: str,
    preflight: Mapping[str, str],
    build: Mapping[str, str],
    source_build: Mapping[str, str],
    provenance: Mapping[str, str],
    expected_branch: str,
    expected_commit: str,
) -> None:
    context = f"{label} clean-source build attestation"
    source_build_root = root / "provenance/build/source-build"
    required_files = (
        "source-build-attestation.txt",
        "source-build-attestation-before.state",
        "source-build-attestation-after.state",
        "source-tree.tar",
        "source-tree-archive-before.state",
        "source-tree-archive-after.state",
        "fresh-built-binary",
        "fresh-binary-archive-before.state",
        "fresh-binary-archive-after.state",
        "CMakeCache.txt",
        "cmake-cache-archive-before.state",
        "cmake-cache-archive-after.state",
        "source-archive.command.txt",
        "source-archive.argv",
        "source-archive.stdout.log",
        "source-archive.stderr.log",
        "source-extract.command.txt",
        "source-extract.argv",
        "source-extract.stdout.log",
        "source-extract.stderr.log",
        "configure.command.txt",
        "configure.argv",
        "configure.stdout.log",
        "configure.stderr.log",
        "target-build.command.txt",
        "target-build.argv",
        "target-build.stdout.log",
        "target-build.stderr.log",
    )
    require_regular_files(
        root,
        (f"provenance/build/source-build/{name}" for name in required_files),
        context,
    )
    for name in ("configure.stdout.log", "target-build.stdout.log"):
        if (source_build_root / name).stat().st_size == 0:
            raise error(context, f"fresh build evidence is empty: {name}")

    expected_mode = "git_archive_fresh_cmake_clean_first_v1"
    expected_environment = "empty_environment_recorded_toolchain_v1"
    expected_target = "astra_itch_book_replay_benchmark"
    if source_build["attestation_version"] != "1":
        raise error(context, "unknown clean-source build attestation schema")
    if source_build["mode"] != expected_mode:
        raise error(context, "clean-source build did not use the required fresh-build mode")
    if source_build["environment_policy"] != expected_environment:
        raise error(context, "clean-source build did not use the required empty environment")
    if source_build["target"] != expected_target:
        raise error(context, "clean-source build identifies another target")
    expected_metadata = {
        "source_build_attestation_version": "1",
        "source_build_mode": expected_mode,
        "source_build_target": expected_target,
    }
    for field, expected in expected_metadata.items():
        if preflight[field] != expected:
            raise error(context, f"preflight {field} differs from the attestation")
        if build[field] != expected:
            raise error(context, f"build provenance {field} differs from the attestation")
    if build["source_build_environment_policy"] != expected_environment:
        raise error(context, "build provenance environment policy differs")
    if source_build["result"] != "PASS" or source_build["error"] != "none":
        raise error(context, "clean-source target build did not pass")
    if (
        source_build["configure_exit_code"] != "0"
        or source_build["target_build_exit_code"] != "0"
    ):
        raise error(context, "clean-source configure or target build did not exit successfully")

    expected_identity = {
        "source_commit": expected_commit,
        "source_parent": preflight["git_parent_before"].lower(),
        "source_tree": preflight["git_tree_before"].lower(),
        "source_branch": expected_branch,
        "source_fingerprint_before": preflight["git_fingerprint_before"],
        "source_fingerprint_after": preflight["git_fingerprint_before"],
    }
    for field, expected in expected_identity.items():
        if source_build[field].lower() != expected.lower():
            raise error(context, f"{field} differs from the verified clean source identity")
    uint(source_build["source_date_epoch"], context, "source_date_epoch")

    attestation_path = source_build_root / "source-build-attestation.txt"
    source_archive_path = source_build_root / "source-tree.tar"
    fresh_binary_path = source_build_root / "fresh-built-binary"
    fresh_cache_path = source_build_root / "CMakeCache.txt"
    actual_attestation_sha = sha256_file(attestation_path)
    actual_source_archive_sha = sha256_file(source_archive_path)
    actual_fresh_binary_sha = sha256_file(fresh_binary_path)
    actual_fresh_cache_sha = sha256_file(fresh_cache_path)
    binary_sha = preflight["binary_sha256"]

    hash_groups = (
        (
            "clean-source build attestation",
            (
                actual_attestation_sha,
                preflight["source_build_attestation_sha256"],
                provenance["source_build_attestation_sha256_before"],
                provenance["source_build_attestation_sha256_after"],
            ),
        ),
        (
            "clean-source tree archive",
            (
                actual_source_archive_sha,
                source_build["source_archive_sha256"],
                preflight["source_tree_archive_sha256"],
                provenance["source_tree_archive_sha256_before"],
                provenance["source_tree_archive_sha256_after"],
            ),
        ),
        (
            "fresh clean-source binary",
            (
                actual_fresh_binary_sha,
                source_build["supplied_binary_sha256"],
                source_build["fresh_binary_sha256"],
                source_build["fresh_binary_archive_sha256"],
                preflight["fresh_binary_sha256"],
                preflight["fresh_binary_archive_sha256"],
                provenance["fresh_binary_archive_sha256_before"],
                provenance["fresh_binary_archive_sha256_after"],
                binary_sha,
                preflight["binary_archive_sha256"],
            ),
        ),
        (
            "fresh clean-source CMake cache",
            (
                actual_fresh_cache_sha,
                source_build["fresh_cmake_cache_sha256"],
                provenance["fresh_cmake_cache_sha256_before"],
                provenance["fresh_cmake_cache_sha256_after"],
            ),
        ),
    )
    for description, values in hash_groups:
        if any(HEX64_RE.fullmatch(value) is None for value in values):
            raise error(context, f"{description} contains a malformed SHA-256")
        if len(set(values)) != 1:
            raise error(context, f"{description} hashes do not prove one identical artifact")

    argv_files = {
        "source_archive_argv_sha256": source_build_root / "source-archive.argv",
        "source_extract_argv_sha256": source_build_root / "source-extract.argv",
        "configure_argv_sha256": source_build_root / "configure.argv",
        "target_build_argv_sha256": source_build_root / "target-build.argv",
    }
    for field, path in argv_files.items():
        expected_hash = source_build[field]
        if HEX64_RE.fullmatch(expected_hash) is None or sha256_file(path) != expected_hash:
            raise error(context, f"{field} does not identify its retained argv")

    git_command = source_build["git_command"]
    tar_command = source_build["tar_command"]
    cmake_command = source_build["cmake_command"]
    env_command = source_build["env_command"]
    source_root = source_build["source_root"]
    archive_output = source_build["source_archive_output"]
    temporary_root = source_build["temporary_root"].rstrip("/")
    temporary_source = source_build["temporary_source"]
    temporary_build = source_build["temporary_build"]
    for field, path in (
        ("git_command", git_command),
        ("tar_command", tar_command),
        ("cmake_command", cmake_command),
        ("env_command", env_command),
        ("source_root", source_root),
        ("source_archive_output", archive_output),
        ("temporary_root", temporary_root),
        ("temporary_source", temporary_source),
        ("temporary_build", temporary_build),
    ):
        if not path.startswith("/"):
            raise error(context, f"{field} is not an absolute path")
    if temporary_source != f"{temporary_root}/source":
        raise error(context, "temporary source path is outside the fresh-build root")
    if temporary_build != f"{temporary_root}/build":
        raise error(context, "temporary build path is outside the fresh-build root")

    source_archive_argv = read_nul_argv(
        source_build_root / "source-archive.argv", context
    )
    validate_shell_command(
        source_build_root / "source-archive.command.txt",
        source_archive_argv,
        context,
    )
    expected_source_archive_argv = (
        git_command,
        "-C",
        source_root,
        "archive",
        "--format=tar",
        f"--output={archive_output}",
        expected_commit,
    )
    if source_archive_argv != expected_source_archive_argv:
        raise error(context, "source archive command does not export the pinned commit")

    source_extract_argv = read_nul_argv(
        source_build_root / "source-extract.argv", context
    )
    validate_shell_command(
        source_build_root / "source-extract.command.txt",
        source_extract_argv,
        context,
    )
    expected_source_extract_argv = (
        tar_command,
        "-xf",
        archive_output,
        "-C",
        temporary_source,
    )
    if source_extract_argv != expected_source_extract_argv:
        raise error(context, "source extraction command does not use the retained archive")

    configure_argv = read_nul_argv(source_build_root / "configure.argv", context)
    validate_shell_command(
        source_build_root / "configure.command.txt", configure_argv, context
    )
    if len(configure_argv) < 9:
        raise error(context, "clean-source configure argv is incomplete")
    environment_prefix = configure_argv[:9]
    if (
        environment_prefix[0:3]
        != (env_command, "-i", f"HOME={temporary_root}/home")
        or not environment_prefix[3].startswith("PATH=")
        or environment_prefix[3] == "PATH="
        or environment_prefix[4:] != (
            f"TMPDIR={temporary_root}/tmp",
            "LC_ALL=C",
            f"SOURCE_DATE_EPOCH={source_build['source_date_epoch']}",
            "ZERO_AR_DATE=1",
            cmake_command,
        )
    ):
        raise error(context, "clean-source build environment is not the required empty environment")

    build_values = {
        field: decode_shell_word(build[field], context, field)
        for field in (
            "cmake_make_program",
            "cmake_cxx_compiler",
            "cmake_cxx_flags",
            "cmake_cxx_flags_release",
            "cmake_exe_linker_flags",
            "cmake_exe_linker_flags_release",
            "cmake_static_linker_flags",
            "cmake_static_linker_flags_release",
        )
    }
    expected_fresh_cache = {
        "CMAKE_HOME_DIRECTORY": temporary_source,
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_GENERATOR": build["cmake_generator"],
        "CMAKE_MAKE_PROGRAM": build_values["cmake_make_program"],
        "CMAKE_CXX_COMPILER": build_values["cmake_cxx_compiler"],
        "CMAKE_CXX_FLAGS": build_values["cmake_cxx_flags"],
        "CMAKE_CXX_FLAGS_RELEASE": build_values["cmake_cxx_flags_release"],
        "CMAKE_EXE_LINKER_FLAGS": build_values["cmake_exe_linker_flags"],
        "CMAKE_EXE_LINKER_FLAGS_RELEASE": build_values[
            "cmake_exe_linker_flags_release"
        ],
        "CMAKE_STATIC_LINKER_FLAGS": build_values["cmake_static_linker_flags"],
        "CMAKE_STATIC_LINKER_FLAGS_RELEASE": build_values[
            "cmake_static_linker_flags_release"
        ],
        "ASTRA_BUILD_APPS": "OFF",
        "ASTRA_BUILD_TESTS": "OFF",
        "ASTRA_BUILD_BENCHMARKS": "ON",
        "ASTRA_ENABLE_DPDK": "OFF",
        "ASTRA_ENABLE_IPO": "OFF",
    }
    fresh_cache = cmake_cache_values(
        fresh_cache_path, expected_fresh_cache.keys(), context
    )
    for field, expected in expected_fresh_cache.items():
        if fresh_cache[field] != expected:
            raise error(context, f"fresh CMake cache {field} differs from configure argv")

    expected_configure_tail = (
        "-S",
        temporary_source,
        "-B",
        temporary_build,
        "-G",
        build["cmake_generator"],
        "--no-warn-unused-cli",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_MAKE_PROGRAM={build_values['cmake_make_program']}",
        f"-DCMAKE_CXX_COMPILER={build_values['cmake_cxx_compiler']}",
        f"-DCMAKE_CXX_FLAGS={build_values['cmake_cxx_flags']}",
        f"-DCMAKE_CXX_FLAGS_RELEASE={build_values['cmake_cxx_flags_release']}",
        f"-DCMAKE_EXE_LINKER_FLAGS={build_values['cmake_exe_linker_flags']}",
        f"-DCMAKE_EXE_LINKER_FLAGS_RELEASE={build_values['cmake_exe_linker_flags_release']}",
        f"-DCMAKE_STATIC_LINKER_FLAGS={build_values['cmake_static_linker_flags']}",
        f"-DCMAKE_STATIC_LINKER_FLAGS_RELEASE={build_values['cmake_static_linker_flags_release']}",
        "-DASTRA_BUILD_APPS=OFF",
        "-DASTRA_BUILD_TESTS=OFF",
        "-DASTRA_BUILD_BENCHMARKS=ON",
        "-DASTRA_ENABLE_DPDK=OFF",
        "-DASTRA_ENABLE_IPO=OFF",
    )
    if configure_argv[9:] != expected_configure_tail:
        raise error(context, "clean-source configure command differs from the reviewed build graph")

    target_build_argv = read_nul_argv(
        source_build_root / "target-build.argv", context
    )
    validate_shell_command(
        source_build_root / "target-build.command.txt",
        target_build_argv,
        context,
    )
    expected_target_build_argv = environment_prefix + (
        "--build",
        temporary_build,
        "--target",
        expected_target,
        "--clean-first",
        "--verbose",
    )
    if target_build_argv != expected_target_build_argv:
        raise error(context, "target build command is not a clean-first build of the replay target")


def validate_capacity_profile_artifact(
    root: Path,
    label: str,
    preflight: Mapping[str, str],
    provenance: Mapping[str, str],
    schema: str,
) -> Mapping[str, str]:
    context = f"{label} capacity profile"
    archive_relative = "provenance/capacity-evidence-manifest.txt"
    state_relatives = (
        "provenance/capacity-evidence-source-before.state",
        "provenance/capacity-evidence-source-after.state",
        "provenance/capacity-evidence-archive-before.state",
        "provenance/capacity-evidence-archive-after.state",
    )
    provenance_fields = (
        "capacity_evidence_sha256_before",
        "capacity_evidence_sha256_after",
        "capacity_evidence_archive_sha256_before",
        "capacity_evidence_archive_sha256_after",
    )

    if schema == BRANCH5_NATIVE_SCHEMA:
        pinned_trace = preflight["trace_sha256"] == BRANCH5_PINNED_TRACE_SHA256
        expected = {
            field: "not_applicable"
            for field in CAPACITY_PREFLIGHT_FIELDS
            if field != "capacity_profile_policy"
        }
        expected["capacity_profile_policy"] = "not_applicable_branch5_native_v1"
        if pinned_trace:
            expected["capacity_evidence_source_sha256"] = (
                BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256
            )
            expected["capacity_evidence_archive_sha256"] = (
                BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256
            )
        for field, value in expected.items():
            if preflight[field] != value:
                raise error(
                    context,
                    f"branch5 {field} differs from the required capacity identity",
                )
        expected_provenance = (
            BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256
            if pinned_trace
            else "not_applicable"
        )
        if any(
            provenance[field] != expected_provenance
            for field in provenance_fields
        ):
            raise error(context, "branch5 capacity evidence provenance is invalid")
        if pinned_trace:
            require_regular_files(
                root, (archive_relative,) + state_relatives, context
            )
            source_before = validate_capacity_capture_state(
                root / state_relatives[0],
                BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256,
                f"{context} source-before state",
            )
            source_after = validate_capacity_capture_state(
                root / state_relatives[1],
                BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256,
                f"{context} source-after state",
            )
            archive_before = validate_capacity_capture_state(
                root / state_relatives[2],
                BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256,
                f"{context} archive-before state",
            )
            archive_after = validate_capacity_capture_state(
                root / state_relatives[3],
                BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256,
                f"{context} archive-after state",
            )
            if source_before != source_after:
                raise error(
                    context,
                    "branch5 capacity evidence source changed during acceptance",
                )
            if archive_before != archive_after:
                raise error(
                    context,
                    "branch5 archived capacity evidence changed during acceptance",
                )
            if source_before[0] == archive_before[0]:
                raise error(
                    context,
                    "branch5 capacity source and retained archive paths are identical",
                )
            if not archive_before[0].endswith("/" + archive_relative):
                raise error(
                    context,
                    "captured branch5 capacity archive path is not fixed",
                )
            archive_sha = hashlib.sha256((root / archive_relative).read_bytes()).hexdigest()
            if archive_sha != BRANCH5_PINNED_CAPACITY_EVIDENCE_SHA256:
                raise error(
                    context,
                    "reviewed branch5 capacity manifest hash differs",
                )
        elif (root / archive_relative).exists() or any(
            (root / relative).exists() for relative in state_relatives
        ):
            raise error(
                context,
                "unbound branch5 artifact unexpectedly carries pinned capacity evidence",
            )
        return {}

    if preflight["capacity_profile_bound"] != "1":
        raise error(context, "redesign artifact does not carry a bound capacity profile")
    for field in (
        "capacity_evidence_sha256",
        "capacity_corpus_manifest_sha256",
        "capacity_profiler_sha256",
    ):
        if HEX64_RE.fullmatch(preflight[field]) is None:
            raise error(context, f"{field} is not lowercase SHA-256")
    if CAPACITY_PROFILE_RE.fullmatch(preflight["capacity_profile_name"]) is None:
        raise error(context, "capacity profile name is invalid")

    numbers = {
        field: positive_uint(preflight[field], context, field)
        for field in (
            "capacity_profiled_max_order_ref",
            "capacity_profiled_unique_price_pages",
            "capacity_minimum_direct_order_headroom",
            "capacity_effective_direct_order_headroom",
            "capacity_minimum_price_page_headroom",
            "capacity_effective_price_page_headroom",
        )
    }
    plan_lines = [
        line
        for line in read_text(root / "storage-plan.stdout.log", context).splitlines()
        if line.strip()
    ]
    plan = one_record(plan_lines, "itch_book_replay_storage_plan", context)
    require_fields(
        plan,
        CAPACITY_REPLAY_FIELDS
        + ("direct_order_slots", "fallback_buckets", "price_page_capacity"),
        context,
    )
    for field in CAPACITY_REPLAY_FIELDS:
        if plan[field] != preflight[field]:
            raise error(context, f"storage plan {field} differs from preflight")
    direct_slots = positive_uint(plan["direct_order_slots"], context, "direct_order_slots")
    fallback_buckets = positive_uint(plan["fallback_buckets"], context, "fallback_buckets")
    price_capacity = positive_uint(plan["price_page_capacity"], context, "price_page_capacity")
    if fallback_buckets > 0xFFFFFFFF or fallback_buckets & (fallback_buckets - 1):
        raise error(context, "planned fallback bucket count is not a uint32 power of two")
    direct_headroom = direct_slots - (numbers["capacity_profiled_max_order_ref"] + 1)
    if (
        direct_headroom != numbers["capacity_effective_direct_order_headroom"]
        or direct_headroom < numbers["capacity_minimum_direct_order_headroom"]
    ):
        raise error(context, "planned direct table is not bound to profile headroom")
    price_headroom = price_capacity - numbers["capacity_profiled_unique_price_pages"]
    if (
        price_headroom != numbers["capacity_effective_price_page_headroom"]
        or price_headroom < numbers["capacity_minimum_price_page_headroom"]
    ):
        raise error(context, "planned price pages are not bound to profile headroom")

    policy = preflight["capacity_profile_policy"]
    if policy == "canonical_manifest_v1":
        if preflight["capacity_evidence_schema"] != CAPACITY_MANIFEST_SCHEMA:
            raise error(context, "custom capacity manifest schema is invalid")
        expected_sha = preflight["capacity_evidence_sha256"]
        for field in (
            "capacity_evidence_source_sha256",
            "capacity_evidence_archive_sha256",
        ):
            if preflight[field] != expected_sha:
                raise error(context, f"{field} differs from computed capacity identity")
        if any(provenance[field] != expected_sha for field in provenance_fields):
            raise error(context, "custom capacity manifest changed before/after acceptance")
        require_regular_files(root, (archive_relative,) + state_relatives, context)
        source_before = validate_capacity_capture_state(
            root / state_relatives[0],
            expected_sha,
            f"{context} source-before state",
        )
        source_after = validate_capacity_capture_state(
            root / state_relatives[1],
            expected_sha,
            f"{context} source-after state",
        )
        archive_before = validate_capacity_capture_state(
            root / state_relatives[2],
            expected_sha,
            f"{context} archive-before state",
        )
        archive_after = validate_capacity_capture_state(
            root / state_relatives[3],
            expected_sha,
            f"{context} archive-after state",
        )
        if source_before != source_after:
            raise error(context, "capacity evidence source state changed during acceptance")
        if archive_before != archive_after:
            raise error(context, "archived capacity evidence state changed during acceptance")
        if source_before[0] == archive_before[0]:
            raise error(context, "capacity source and retained archive paths are identical")
        if not archive_before[0].endswith("/" + archive_relative):
            raise error(context, "captured capacity archive path is not the fixed artifact path")
        manifest = parse_capacity_manifest(root / archive_relative, context)
        if manifest["manifest_sha256"] != expected_sha:
            raise error(context, "archived capacity manifest hash differs from preflight")
        expected_manifest_relations = {
            "profile_name": "capacity_profile_name",
            "corpus_manifest_sha256": "capacity_corpus_manifest_sha256",
            "profiler_sha256": "capacity_profiler_sha256",
            "profiled_max_order_ref": "capacity_profiled_max_order_ref",
            "profiled_unique_price_pages": "capacity_profiled_unique_price_pages",
            "minimum_direct_order_headroom": "capacity_minimum_direct_order_headroom",
            "minimum_price_page_headroom": "capacity_minimum_price_page_headroom",
        }
        for manifest_field, preflight_field in expected_manifest_relations.items():
            if manifest[manifest_field] != preflight[preflight_field]:
                raise error(
                    context,
                    f"manifest {manifest_field} differs from measured profile",
                )
        for manifest_field, plan_value in (
            ("order_direct_slots", direct_slots),
            ("order_fallback_buckets", fallback_buckets),
            ("price_page_capacity", price_capacity),
        ):
            if int(manifest[manifest_field]) != plan_value:
                raise error(context, f"manifest {manifest_field} differs from storage plan")
    elif policy == "pinned_trace_builtin_v1":
        if (
            preflight["capacity_evidence_schema"] != PINNED_CAPACITY_SCHEMA
            or preflight["capacity_profile_name"] != PINNED_CAPACITY_PROFILE
            or preflight["capacity_evidence_sha256"] != preflight["trace_sha256"]
            or preflight["capacity_corpus_manifest_sha256"] != preflight["trace_sha256"]
            or preflight["capacity_profiler_sha256"]
            != PINNED_CAPACITY_PROFILER_SHA256
        ):
            raise error(context, "pinned capacity identity differs from pinned trace")
        for field, expected in PINNED_CAPACITY_REPLAY_FIELDS.items():
            if preflight[field] != expected:
                raise error(
                    context,
                    f"pinned {field} differs from the reviewed built-in profile",
                )
        pinned_plan = {
            "direct_order_slots": str(direct_slots),
            "fallback_buckets": str(fallback_buckets),
            "price_page_capacity": str(price_capacity),
        }
        for field, expected in PINNED_CAPACITY_PLAN_FIELDS.items():
            if pinned_plan[field] != expected:
                raise error(
                    context,
                    f"pinned {field} differs from the reviewed built-in profile",
                )
        for field in (
            "capacity_evidence_source_sha256",
            "capacity_evidence_archive_sha256",
        ):
            if preflight[field] != "not_applicable":
                raise error(context, f"pinned profile {field} is not N/A")
        if any(provenance[field] != "not_applicable" for field in provenance_fields):
            raise error(context, "pinned capacity file provenance is not N/A")
        if (root / archive_relative).exists() or any(
            (root / relative).exists() for relative in state_relatives
        ):
            raise error(context, "pinned profile unexpectedly carries a custom manifest")
    else:
        raise error(context, f"unknown capacity-profile policy: {policy!r}")
    return {
        "direct_order_slots": str(direct_slots),
        "fallback_buckets": str(fallback_buckets),
        "price_page_capacity": str(price_capacity),
    }


def validate_branch5_capacity_artifact(
    root: Path,
    label: str,
    preflight: Mapping[str, str],
) -> None:
    context = f"{label} branch5 capacity"
    pinned_trace = preflight["trace_sha256"] == BRANCH5_PINNED_TRACE_SHA256
    expected_policy = (
        "pinned_trace_canonical_v1"
        if pinned_trace
        else "default_binary_unbound_trace_v1"
    )
    expected_profile = (
        BRANCH5_PINNED_CAPACITY_PROFILE
        if pinned_trace
        else "branch5-default-binary-v1"
    )
    expected_evidence_policy = (
        "reviewed_manifest_sha256_v1" if pinned_trace else "unbound_trace_v1"
    )
    for field, expected in (
        ("admission_basis", "explicit_override_plus_reserve"),
        ("branch5_capacity_policy", expected_policy),
        ("branch5_capacity_profile_name", expected_profile),
        ("branch5_capacity_evidence_policy", expected_evidence_policy),
        ("branch5_plan_scope", "shared_price_pool_only"),
        ("branch5_per_book_bytes_resolved_at_ready", "1"),
    ):
        if preflight[field] != expected:
            raise error(context, f"{field} differs from the canonical branch5 policy")

    runtime_mode = preflight["branch5_runtime_capacity_mode"]
    if runtime_mode not in ("binary_defaults_v1", "canonical_explicit_v1"):
        raise error(context, "branch5 runtime capacity mode is invalid")
    if pinned_trace and runtime_mode != "canonical_explicit_v1":
        raise error(
            context,
            "pinned branch5 trace did not use all four explicit canonical capacity flags",
        )

    price_pool = positive_uint(
        preflight["plan_price_pool_bytes"], context, "plan_price_pool_bytes"
    )
    derived = positive_uint(
        preflight["derived_planned_storage_bytes"],
        context,
        "derived_planned_storage_bytes",
    )
    planned = positive_uint(preflight["planned_bytes"], context, "planned_bytes")
    reserve = uint(preflight["reserve_bytes"], context, "reserve_bytes")
    required = positive_uint(preflight["required_bytes"], context, "required_bytes")
    if (
        price_pool != BRANCH5_PINNED_PRICE_POOL_BYTES
        or derived != price_pool
        or required != planned + reserve
    ):
        raise error(context, "branch5 admission arithmetic differs from its storage plan")
    if pinned_trace and planned != BRANCH5_PINNED_PLANNED_BYTES:
        raise error(
            context,
            "pinned branch5 trace does not carry the canonical 64 GiB admission plan",
        )
    if pinned_trace and (
        reserve != BRANCH5_PINNED_RESERVE_BYTES
        or preflight["sample_every"] != str(BRANCH5_PINNED_SAMPLE_EVERY)
        or preflight["sample_capacity_override"]
        != str(BRANCH5_PINNED_SAMPLE_CAPACITY)
    ):
        raise error(
            context,
            "pinned branch5 trace does not carry the canonical reserve/sample plan",
        )

    for field, expected in BRANCH5_PINNED_PRICE_VECTOR_FIELDS.items():
        preflight_field = f"plan_{field}"
        if preflight[preflight_field] != expected:
            raise error(context, f"{preflight_field} differs from the pinned price pool")
    for field, expected in BRANCH5_PINNED_PLAN_FIELDS.items():
        preflight_field = f"plan_{field}"
        if preflight[preflight_field] != expected:
            raise error(context, f"{preflight_field} differs from the pinned capacity")

    plan_lines = [
        line
        for line in read_text(root / "storage-plan.stdout.log", context).splitlines()
        if line.strip()
    ]
    plan = one_record(plan_lines, "itch_book_replay_storage_plan", context)
    required_plan_fields = (
        "hot_arena_schema",
        "planned_price_pool_bytes",
        *BRANCH5_PINNED_PRICE_VECTOR_FIELDS,
        *BRANCH5_PINNED_PLAN_FIELDS,
    )
    require_fields(plan, required_plan_fields, context)
    if plan["hot_arena_schema"] != BRANCH5_NATIVE_SCHEMA:
        raise error(context, "branch5 storage plan reports another arena schema")
    if plan["planned_price_pool_bytes"] != str(BRANCH5_PINNED_PRICE_POOL_BYTES):
        raise error(context, "branch5 storage plan price-pool total is noncanonical")
    for field, expected in (
        *BRANCH5_PINNED_PRICE_VECTOR_FIELDS.items(),
        *BRANCH5_PINNED_PLAN_FIELDS.items(),
    ):
        if plan[field] != expected:
            raise error(context, f"branch5 storage plan {field} is noncanonical")

    capacity_prefixes = (
        "--default-order-capacity=",
        "--price-node-capacity=",
        "--price-leaf-capacity=",
        "--price-level-capacity=",
    )
    try:
        command_tokens = shlex.split(
            read_text(root / "storage-plan.command.txt", context)
        )
    except ValueError as exc:
        raise error(context, f"cannot parse branch5 storage-plan command: {exc}") from exc
    observed: Dict[str, str] = {}
    sample_capacity_flags: List[str] = []
    for token in command_tokens:
        if token.startswith("--sample-capacity="):
            sample_capacity_flags.append(
                token[len("--sample-capacity=") :]
            )
        for prefix in capacity_prefixes:
            if token.startswith(prefix):
                if prefix in observed:
                    raise error(context, f"duplicate branch5 capacity flag {prefix}")
                observed[prefix] = token[len(prefix) :]
    expected_flags = {
        "--default-order-capacity=": BRANCH5_PINNED_PLAN_FIELDS[
            "default_order_capacity"
        ],
        "--price-node-capacity=": BRANCH5_PINNED_PLAN_FIELDS[
            "price_internal_node_capacity"
        ],
        "--price-leaf-capacity=": BRANCH5_PINNED_PLAN_FIELDS[
            "price_leaf_capacity"
        ],
        "--price-level-capacity=": BRANCH5_PINNED_PLAN_FIELDS[
            "price_level_capacity"
        ],
    }
    if runtime_mode == "binary_defaults_v1":
        if observed:
            raise error(context, "branch5 command hides runtime flags behind defaults mode")
    elif observed != expected_flags:
        raise error(
            context,
            "branch5 explicit runtime capacity flags are partial or noncanonical",
        )
    expected_sample_override = preflight["sample_capacity_override"]
    if expected_sample_override == "binary-default":
        if sample_capacity_flags:
            raise error(
                context,
                "branch5 command sample-capacity flag differs from defaults mode",
            )
    else:
        positive_uint(
            expected_sample_override, context, "sample_capacity_override"
        )
        if sample_capacity_flags != [expected_sample_override]:
            raise error(
                context,
                "branch5 command lacks its exact explicit sample-capacity flag",
            )


def validate_provenance(
    root: Path,
    label: str,
    preflight: Mapping[str, str],
    build: Mapping[str, str],
    source_build: Mapping[str, str],
    provenance: Mapping[str, str],
    expected_branch: str,
    expected_commit: str,
) -> None:
    context = f"{label} provenance"
    if preflight["git_dirty"] != "0":
        raise error(context, "artifact was produced from a dirty worktree")
    if provenance["provenance_version"] != "2":
        raise error(context, "unknown provenance verification schema")
    if preflight["git_branch_before"] != expected_branch:
        raise error(context, f"branch {preflight['git_branch_before']!r} != expected {expected_branch!r}")
    if preflight["git_commit_before"].lower() != expected_commit:
        raise error(context, "preflight commit differs from the explicit expected commit")
    if COMMIT_RE.fullmatch(preflight["git_parent_before"].lower()) is None:
        raise error(context, "preflight parent commit is malformed")
    if COMMIT_RE.fullmatch(preflight["git_tree_before"].lower()) is None:
        raise error(context, "preflight tree identity is malformed")
    if provenance["result"] != "PASS" or provenance["error"] != "none":
        raise error(context, "pre/post provenance verification did not pass cleanly")
    for phase in ("before", "after"):
        if provenance[f"git_commit_{phase}"].lower() != expected_commit:
            raise error(context, f"git commit {phase} differs from expected pinned commit")
        if provenance[f"git_branch_{phase}"] != expected_branch:
            raise error(context, f"git branch {phase} differs from expected branch")
        if provenance[f"git_parent_{phase}"].lower() != preflight["git_parent_before"].lower():
            raise error(context, f"git parent {phase} differs from preflight parent")
        if provenance[f"git_tree_{phase}"].lower() != preflight["git_tree_before"].lower():
            raise error(context, f"git tree {phase} differs from preflight tree")
    if provenance["git_fingerprint_before"] != provenance["git_fingerprint_after"]:
        raise error(context, "Git fingerprint changed during acceptance")
    if provenance["git_fingerprint_before"] != preflight["git_fingerprint_before"]:
        raise error(context, "preflight/provenance Git fingerprints differ")

    expected_hash_relations = (
        ("binary_sha256_before", "binary_sha256_after", "binary_sha256"),
        ("binary_archive_sha256_before", "binary_archive_sha256_after", "binary_archive_sha256"),
        ("trace_sha256_before", "trace_sha256_after", "trace_sha256"),
        ("harness_sha256_before", "harness_sha256_after", "harness_source_sha256"),
        ("harness_archive_sha256_before", "harness_archive_sha256_after", "harness_archive_sha256"),
        (
            "hot_path_verifier_sha256_before",
            "hot_path_verifier_sha256_after",
            "hot_path_verifier_source_sha256",
        ),
    )
    for before, after, preflight_field in expected_hash_relations:
        values = (provenance[before], provenance[after], preflight[preflight_field])
        if any(HEX64_RE.fullmatch(value) is None for value in values) or len(set(values)) != 1:
            raise error(context, f"unstable or malformed hashes for {preflight_field}")
    for field in ("cmake_cache_sha256_before", "cmake_cache_sha256_after"):
        if HEX64_RE.fullmatch(provenance[field]) is None:
            raise error(context, f"{field} is not SHA-256")
    if provenance["cmake_cache_sha256_before"] != provenance["cmake_cache_sha256_after"]:
        raise error(context, "CMake cache changed during acceptance")
    if preflight["binary_sha256"] != preflight["binary_archive_sha256"]:
        raise error(context, "tested and archived binary hashes differ")
    if preflight["harness_source_sha256"] != preflight["harness_archive_sha256"]:
        raise error(context, "source and archived harness hashes differ")
    if preflight["hot_path_verifier_source_sha256"] != preflight["hot_path_verifier_archive_sha256"]:
        raise error(context, "source and archived hot-path verifier hashes differ")

    archived_hashes = (
        ("provenance/tested-binary", preflight["binary_sha256"]),
        (
            "provenance/run_order_book_acceptance.sh",
            preflight["harness_source_sha256"],
        ),
        (
            "provenance/verify_order_book_hot_path.sh",
            preflight["hot_path_verifier_source_sha256"],
        ),
        (
            "provenance/build/CMakeCache.txt",
            provenance["cmake_cache_sha256_before"],
        ),
    )
    require_regular_files(root, (name for name, _ in archived_hashes), context)
    for name, expected_hash in archived_hashes:
        if sha256_file(root / name) != expected_hash:
            raise error(context, f"archived file hash differs from provenance: {name}")

    clean_evidence = (
        "../git-status-porcelain.txt",
        "git-status-before.txt",
        "git-status-post-build.txt",
        "git-status-after.txt",
        "git-diff-head-before.patch",
        "git-diff-head-post-build.patch",
        "git-diff-head-after.patch",
        "git-untracked-paths-before.nul",
        "git-untracked-paths-post-build.nul",
        "git-untracked-paths-after.nul",
        "git-untracked-blobs-before.txt",
        "git-untracked-blobs-post-build.txt",
        "git-untracked-blobs-after.txt",
    )
    for name in clean_evidence:
        if name.startswith("../"):
            relative = name[3:]
            path = root / relative
        else:
            relative = f"provenance/{name}"
            path = root / relative
        require_regular_files(root, (relative,), context)
        if path.stat().st_size != 0:
            raise error(context, f"clean-worktree evidence is nonempty: {name}")

    identity_files = (
        ("git-commit-before.txt", expected_commit),
        ("git-commit-post-build.txt", expected_commit),
        ("git-commit-after.txt", expected_commit),
        ("git-parent-before.txt", preflight["git_parent_before"]),
        ("git-parent-post-build.txt", preflight["git_parent_before"]),
        ("git-parent-after.txt", preflight["git_parent_before"]),
        ("git-tree-before.txt", preflight["git_tree_before"]),
        ("git-tree-post-build.txt", preflight["git_tree_before"]),
        ("git-tree-after.txt", preflight["git_tree_before"]),
        ("git-branch-before.txt", expected_branch),
        ("git-branch-post-build.txt", expected_branch),
        ("git-branch-after.txt", expected_branch),
    )
    for name, expected in identity_files:
        relative = f"provenance/{name}"
        require_regular_files(root, (relative,), context)
        observed = read_text(root / relative, context).rstrip("\n")
        if observed != expected:
            raise error(context, f"{name} differs from expected identity")

    validate_source_build_attestation(
        root,
        label,
        preflight,
        build,
        source_build,
        provenance,
        expected_branch,
        expected_commit,
    )

    verifier_log = read_text(root / "provenance/hot-path-verifier.stdout.log", context)
    verifier_lines = [line for line in verifier_log.splitlines() if line.startswith("hot_path_verifier ")]
    expected_verifier_identity = (
        "hot_path_verifier version=2 result=PASS "
        f"schema={preflight['plan_hot_arena_schema']} "
    )
    if len(verifier_lines) != 1 or expected_verifier_identity not in verifier_lines[0]:
        raise error(
            context,
            "hot-path disassembly verifier did not report one matching version-2 PASS",
        )
    if f"binary_sha256={preflight['binary_sha256']}" not in verifier_lines[0]:
        raise error(context, "hot-path verifier identifies another binary")
    if (root / "provenance/hot-path-disassembly.txt").stat().st_size == 0:
        raise error(context, "hot-path disassembly report is empty")


def validate_summary(
    artifact: Artifact,
) -> None:
    context = f"{artifact.label} summary"
    rows = artifact.summary
    latency_rows = [row for row in rows if row["kind"] == "latency"]
    if sorted(row["id"] for row in latency_rows) != list(artifact.run_ids):
        raise error(context, "summary latency run IDs do not match the requested run count")
    for row in latency_rows:
        run_id = row["id"]
        if row["status"] != "PASS" or row["exit_code"] != "0":
            raise error(context, f"latency run {run_id} did not PASS with exit 0")
        output = artifact.latency[run_id].distributions["aggregate"].metrics
        for summary_field, metric in (
            ("p50_ns", "p50_ns"),
            ("p99_ns", "p99_ns"),
            ("p99_9_ns", "p99_9_ns"),
        ):
            if uint(row[summary_field], context, f"{run_id}.{summary_field}") != output[metric]:
                raise error(context, f"run {run_id} summary/log {summary_field} mismatch")

    correctness_rows = [row for row in rows if row["kind"] == "correctness"]
    allowed_correctness = {"discovery", "correctness-verification"}
    if any(row["id"] not in allowed_correctness for row in correctness_rows):
        raise error(context, "summary contains an unknown correctness row")
    verification = exactly_one_summary(rows, "correctness", "correctness-verification", context)
    if verification["status"] != "PASS" or verification["exit_code"] != "0":
        raise error(context, "correctness verification did not PASS with exit 0")
    for row in correctness_rows:
        if row["status"] != "PASS" or row["exit_code"] != "0":
            raise error(context, f"correctness row {row['id']} is not a full PASS")

    hardware = exactly_one_summary(rows, "hardware-counters", "perf-stat", context)
    if hardware["status"] != "PASS" or hardware["exit_code"] != "0":
        raise error(context, "hardware-counter replay was skipped or failed")
    provenance = exactly_one_summary(rows, "provenance", "pre-post", context)
    if provenance["status"] != "PASS" or provenance["exit_code"] != "0":
        raise error(context, "summary provenance row did not PASS")
    overall = exactly_one_summary(rows, "overall", "-", context)
    if overall["status"] != "PASS" or overall["exit_code"] != "0":
        raise error(context, "overall acceptance row did not PASS")
    worst = exactly_one_summary(rows, "worst", "all-latency-runs", context)
    expected_worst = {
        metric: max(
            artifact.latency[run_id].distributions["aggregate"].metrics[metric]
            for run_id in artifact.run_ids
        )
        for metric in ("p50_ns", "p99_ns", "p99_9_ns")
    }
    for metric, expected in expected_worst.items():
        if uint(worst[metric], context, f"worst.{metric}") != expected:
            raise error(context, f"worst summary {metric} differs from run logs")

    expected_row_count = len(artifact.run_ids) + len(correctness_rows) + 4
    if len(rows) != expected_row_count:
        raise error(context, "summary contains missing or unexpected rows")


def load_artifact(
    root: Path,
    label: str,
    expected_branch: str,
    expected_commit: str,
) -> Artifact:
    verify_manifest(root, label)
    require_regular_files(
        root,
        (
            "preflight.txt",
            "summary.tsv",
            "provenance/provenance-verification.txt",
            "provenance/build/build-provenance.txt",
            "provenance/build/source-build/source-build-attestation.txt",
            "provenance/build/compiler-version.txt",
            "provenance/hot-path-verifier.stdout.log",
            "provenance/hot-path-disassembly.txt",
            "storage-plan.stdout.log",
            "storage-plan.stderr.log",
            "storage-plan.command.txt",
            "benchmark-help.txt",
            "perf-preflight.perf-stat.csv",
            "hardware-counters.command.txt",
            "hardware-counters.stdout.log",
            "hardware-counters.stderr.log",
            "hardware-counters.perf-stat.csv",
            "correctness-verification.stdout.log",
            "correctness-verification.stderr.log",
        ),
        label,
    )
    preflight = unique_equals_fields(root / "preflight.txt", PREFLIGHT_FIELDS, f"{label} preflight")
    build = unique_equals_fields(
        root / "provenance/build/build-provenance.txt", BUILD_FIELDS, f"{label} build provenance"
    )
    source_build = unique_equals_fields(
        root / "provenance/build/source-build/source-build-attestation.txt",
        SOURCE_BUILD_FIELDS,
        f"{label} clean-source build attestation",
    )
    provenance = unique_equals_fields(
        root / "provenance/provenance-verification.txt",
        PROVENANCE_FIELDS,
        f"{label} provenance verification",
    )
    summary = parse_summary(root / "summary.tsv", f"{label} summary")

    for relative in (
        "provenance/build/compiler-version.txt",
        "storage-plan.stdout.log",
        "benchmark-help.txt",
        "perf-preflight.perf-stat.csv",
        "hardware-counters.command.txt",
        "hardware-counters.stdout.log",
        "hardware-counters.perf-stat.csv",
    ):
        if (root / relative).stat().st_size == 0:
            raise error(label, f"required evidence is empty: {relative}")

    if preflight["artifact_schema"] != "astra_order_book_acceptance_v1":
        raise error(label, "unknown acceptance artifact schema")
    hot_arena_schema = preflight["plan_hot_arena_schema"]
    if hot_arena_schema not in (REDESIGN_SCHEMA, BRANCH5_NATIVE_SCHEMA):
        raise error(label, "unknown preflight hot-arena schema")
    if hot_arena_schema == BRANCH5_NATIVE_SCHEMA:
        branch5_preflight = unique_equals_fields(
            root / "preflight.txt",
            BRANCH5_PREFLIGHT_FIELDS,
            f"{label} branch5 preflight",
        )
        preflight = {**preflight, **branch5_preflight}
    if preflight["cmake_build_type"] != "Release" or build["cmake_build_type"] != "Release":
        raise error(label, "artifact is not from a Release build")
    if preflight["trace_hash_policy"] != "mandatory_pre_and_post":
        raise error(label, "trace was not mandatorily hashed before and after replay")
    run_count = uint(preflight["runs"], label, "runs")
    if run_count < 5:
        raise error(label, "artifact must contain at least five latency runs")
    run_ids = run_ids_for_count(run_count)
    if preflight["run_perf_stat"] != "1":
        raise error(label, "hardware-counter replay was not requested")
    if preflight["allow_swap"] != "0" or preflight["swap_total_kb"] != "0":
        raise error(label, "swap was allowed or enabled")
    if preflight["machine"] not in ("x86_64", "amd64"):
        raise error(label, "acceptance artifact is not from x86_64")
    if preflight["cpu_isolation_source"] != "sysfs_domain_isolated":
        raise error(label, "candidate CPU isolation was not proven by sysfs")
    if preflight["cpu_scaling_governor"] not in ("performance", "unavailable"):
        raise error(label, "exposed candidate CPU scaling governor was not performance")
    if BOOT_ID_RE.fullmatch(preflight["boot_id"].lower()) is None:
        raise error(label, "boot_id is malformed")
    if not re.search(r"\[(?:always|madvise)\]", preflight["thp_enabled"]):
        raise error(label, "transparent huge pages were not active in always/madvise mode")

    selected_cpu = uint(preflight["cpu"], label, "cpu")
    monitor_cpu = uint(preflight["monitor_cpu"], label, "monitor_cpu")
    numa_node = uint(preflight["numa_node"], label, "numa_node")
    if selected_cpu == monitor_cpu:
        raise error(label, "candidate and monitor CPUs are identical")
    if selected_cpu not in parse_cpu_list(preflight["cpu_online_list"], label, "cpu_online_list"):
        raise error(label, "candidate CPU is not online")
    if selected_cpu not in parse_cpu_list(preflight["cpu_isolated_list"], label, "cpu_isolated_list"):
        raise error(label, "candidate CPU is not kernel-isolated")
    if selected_cpu not in parse_cpu_list(preflight["node_cpulist"], label, "node_cpulist"):
        raise error(label, "candidate CPU is not in the selected NUMA node")
    if numa_node < 0:  # pragma: no cover - uint() makes this impossible.
        raise error(label, "NUMA node is invalid")

    validate_provenance(
        root,
        label,
        preflight,
        build,
        source_build,
        provenance,
        expected_branch,
        expected_commit,
    )
    expected_capacity = validate_capacity_profile_artifact(
        root, label, preflight, provenance, hot_arena_schema
    )
    pinned_branch5_trace = (
        hot_arena_schema == BRANCH5_NATIVE_SCHEMA
        and preflight["trace_sha256"] == BRANCH5_PINNED_TRACE_SHA256
    )
    if hot_arena_schema == BRANCH5_NATIVE_SCHEMA:
        validate_branch5_capacity_artifact(root, label, preflight)

    latency: Dict[str, ReplayOutput] = {}
    for run_id in run_ids:
        prefix = f"latency-{run_id}"
        validate_memory_evidence(
            root,
            prefix,
            f"{label} {prefix}",
            hot_arena_schema,
            pinned_branch5_trace=pinned_branch5_trace,
        )
        latency[run_id] = parse_replay_output(
            root / f"{prefix}.stdout.log",
            f"{label} {prefix}",
            preflight,
            expected_capacity,
            correctness=False,
        )
    validate_memory_evidence(
        root,
        "correctness-verification",
        f"{label} correctness",
        hot_arena_schema,
        pinned_branch5_trace=pinned_branch5_trace,
    )
    correctness = parse_replay_output(
        root / "correctness-verification.stdout.log",
        f"{label} correctness verification",
        preflight,
        expected_capacity,
        correctness=True,
    )
    correctness_command = read_text(
        root / "correctness-verification.command.txt", f"{label} correctness command"
    )
    expected_semantic_gate = (
        "--expect-semantic-mutation-digest="
        + correctness.main["semantic_mutation_digest"]
    )
    if expected_semantic_gate not in correctness_command.split():
        raise error(
            label,
            "correctness verification command did not gate the observed semantic digest",
        )

    discovery_path = root / "correctness-discovery.stdout.log"
    discovery_rows = [
        row
        for row in summary
        if row["kind"] == "correctness" and row["id"] == "discovery"
    ]
    if discovery_path.exists() != bool(discovery_rows):
        raise error(label, "correctness discovery log/summary presence differs")
    if discovery_path.exists():
        validate_memory_evidence(
            root,
            "correctness-discovery",
            f"{label} correctness discovery",
            hot_arena_schema,
            pinned_branch5_trace=pinned_branch5_trace,
        )
        discovery = parse_replay_output(
            discovery_path,
            f"{label} correctness discovery",
            preflight,
            expected_capacity,
            correctness=True,
        )
        for digest_field in ("mutation_digest", "semantic_mutation_digest"):
            if discovery.main[digest_field] != correctness.main[digest_field]:
                raise error(label, f"correctness discovery/verification {digest_field} differs")

    artifact = Artifact(
        label=label,
        root=root,
        preflight=preflight,
        build=build,
        source_build=source_build,
        provenance=provenance,
        summary=summary,
        run_ids=run_ids,
        latency=latency,
        correctness=correctness,
    )
    validate_summary(artifact)

    reference_counts = {
        workload: latency[run_ids[0]].distributions[workload].sample_count
        for workload in WORKLOADS
    }
    for run_id in run_ids[1:]:
        for workload in WORKLOADS:
            if latency[run_id].distributions[workload].sample_count != reference_counts[workload]:
                raise error(label, f"deterministic sample count changed for {workload} in run {run_id}")
    return artifact


COMPARABLE_PREFLIGHT_FIELDS: Tuple[str, ...] = (
    "artifact_schema",
    "host",
    "kernel",
    "boot_id",
    "machine",
    "cpu_vendor_id",
    "cpu_family",
    "cpu_model",
    "cpu_stepping",
    "cpu_model_name",
    "cpu_online_list",
    "cpu_isolated_list",
    "cpu_isolation_source",
    "cpu_scaling_governor",
    "cpu_scaling_driver",
    "trace_size_bytes",
    "trace_sha256",
    "trace_hash_policy",
    "harness_source_sha256",
    "hot_path_verifier_source_sha256",
    "cmake_build_type",
    "cmake_generator",
    "cmake_cxx_compiler",
    "cmake_cxx_compiler_id",
    "cmake_cxx_compiler_version",
    "runs",
    "cpu",
    "monitor_cpu",
    "numa_node",
    "node_cpulist",
    "node_mem_total_bytes",
    "expected_records",
    "expected_bytes",
    "sample_every",
    "warmup_book_messages",
    "min_samples",
    "allow_swap",
    "run_perf_stat",
    "perf_version",
    "swap_total_kb",
    "vm_overcommit_memory",
    "vm_overcommit_ratio",
    "thp_enabled",
    "thp_defrag",
    "cgroup_memory_version",
    "cgroup_memory_dir",
    "cgroup_finite_limit_count",
    "cgroup_effective_max",
)


def compare_identity(
    baseline: Artifact,
    candidate: Artifact,
    expected_trace: str,
    expected_candidate_capacity_evidence: str,
    expected_records: int,
    expected_bytes: int,
) -> None:
    if baseline.preflight["plan_hot_arena_schema"] != BRANCH5_NATIVE_SCHEMA:
        raise error("comparability", "baseline is not the branch5 native schema")
    if candidate.preflight["plan_hot_arena_schema"] != REDESIGN_SCHEMA:
        raise error("comparability", "candidate is not the redesign schema")
    if (
        candidate.preflight["capacity_evidence_sha256"]
        != expected_candidate_capacity_evidence
    ):
        raise error(
            "comparability",
            "candidate capacity evidence differs from the explicit approved SHA-256",
        )
    if baseline.preflight["git_parent_before"].lower() != BRANCH5_PINNED_COMMIT:
        raise error(
            "comparability",
            "baseline compatibility commit is not directly based on pinned branch 5",
        )
    if baseline.preflight["git_tree_before"].lower() != BRANCH5_COMPATIBILITY_TREE:
        raise error(
            "comparability",
            "baseline tree differs from the reviewed branch5 compatibility port",
        )
    if baseline.preflight["binary_sha256"] == candidate.preflight["binary_sha256"]:
        raise error(
            "comparability",
            "baseline and candidate tested-binary hashes are identical",
        )
    for field in COMPARABLE_PREFLIGHT_FIELDS:
        if baseline.preflight[field] != candidate.preflight[field]:
            raise error(
                "comparability",
                f"preflight {field} differs: baseline={baseline.preflight[field]!r} "
                f"candidate={candidate.preflight[field]!r}",
            )
    for field in BUILD_FIELDS:
        if baseline.build[field] != candidate.build[field]:
            raise error(
                "comparability",
                f"build provenance {field} differs: baseline={baseline.build[field]!r} "
                f"candidate={candidate.build[field]!r}",
            )
    baseline_compiler = (baseline.root / "provenance/build/compiler-version.txt").read_bytes()
    candidate_compiler = (candidate.root / "provenance/build/compiler-version.txt").read_bytes()
    if baseline_compiler != candidate_compiler:
        raise error("comparability", "compiler --version evidence differs")
    if baseline.preflight["trace_sha256"] != expected_trace:
        raise error("comparability", "artifact trace SHA-256 differs from explicit expected trace")
    if uint(baseline.preflight["expected_records"], "comparability", "expected_records") != expected_records:
        raise error("comparability", "artifact record count differs from explicit expected count")
    if uint(baseline.preflight["expected_bytes"], "comparability", "expected_bytes") != expected_bytes:
        raise error("comparability", "artifact byte count differs from explicit expected count")
    if uint(baseline.preflight["trace_size_bytes"], "comparability", "trace_size_bytes") != expected_bytes:
        raise error("comparability", "trace filesystem size differs from explicit expected bytes")
    if baseline.preflight["harness_source_sha256"] != candidate.preflight["harness_source_sha256"]:
        raise error("comparability", "acceptance harness hashes differ")

    baseline_sample_capacities = {
        replay.main["sample_capacity"]
        for replay in (*baseline.latency.values(), baseline.correctness)
    }
    candidate_sample_capacities = {
        replay.main["sample_capacity"]
        for replay in (*candidate.latency.values(), candidate.correctness)
    }
    if (
        len(baseline_sample_capacities) != 1
        or len(candidate_sample_capacities) != 1
        or baseline_sample_capacities != candidate_sample_capacities
    ):
        raise error(
            "comparability",
            "effective sample_capacity differs within or between artifacts",
        )

    baseline_semantic = baseline.correctness.main["semantic_mutation_digest"]
    candidate_semantic = candidate.correctness.main["semantic_mutation_digest"]
    if baseline_semantic != candidate_semantic:
        raise error(
            "correctness",
            f"semantic mutation digests differ: baseline={baseline_semantic} candidate={candidate_semantic}",
        )
    if baseline.correctness.main["semantic_mutation_digest_schema"] != candidate.correctness.main["semantic_mutation_digest_schema"]:
        raise error("correctness", "semantic mutation digest schemas differ")

    for field in ("prelude_records", "prelude_bytes"):
        if baseline.correctness.main[field] != candidate.correctness.main[field]:
            raise error(
                "comparability",
                f"correctness System-S boundary {field} differs",
            )

    reference_counts = {
        workload: baseline.latency[baseline.run_ids[0]].distributions[workload].sample_count
        for workload in WORKLOADS
    }
    if baseline.run_ids != candidate.run_ids:
        raise error("comparability", "baseline and candidate run ID sets differ")
    for run_id in baseline.run_ids:
        for workload in WORKLOADS:
            candidate_count = candidate.latency[run_id].distributions[workload].sample_count
            baseline_count = baseline.latency[run_id].distributions[workload].sample_count
            expected_count = reference_counts[workload]
            if baseline_count != expected_count or candidate_count != expected_count:
                raise error(
                    "comparability",
                    f"sample count mismatch for run {run_id} workload {workload}",
                )
        for field in (
            "records",
            "bytes",
            "prelude_records",
            "prelude_bytes",
            "book_messages",
            "applied_book_mutations",
        ):
            if baseline.latency[run_id].main[field] != candidate.latency[run_id].main[field]:
                raise error("comparability", f"run {run_id} {field} differs")


def ratio(candidate: int, baseline: int) -> str:
    if baseline == 0:
        return "1.000000" if candidate == 0 else "inf"
    with localcontext() as context:
        context.prec = max(len(str(candidate)), len(str(baseline))) + 12
        return f"{Decimal(candidate) / Decimal(baseline):.6f}"


def emit_comparison(baseline: Artifact, candidate: Artifact) -> bool:
    tail_limits = {
        workload: {
            metric: max(
                baseline.latency[run_id].distributions[workload].metrics[metric]
                for run_id in baseline.run_ids
            )
            for metric in ("p99_ns", "p99_9_ns")
        }
        for workload in WORKLOADS
    }
    print(
        "policy\tcandidate aggregate p50 in every run <= 150 ns; for every workload "
        "(aggregate,A,F,E,C,X,D,U), candidate p99 and p99.9 in every run <= the "
        f"baseline worst value across all {len(baseline.run_ids)} runs for that workload; "
        "p90 and max are "
        "reported but are not gates"
    )
    print(
        "run\tworkload\tsample_count\tmetric\tbaseline_ns\tcandidate_ns\t"
        "candidate_over_baseline\tdelta_ns\tgate\tlimit_ns\tresult"
    )
    passed = True
    for run_id in baseline.run_ids:
        for workload in WORKLOADS:
            baseline_distribution = baseline.latency[run_id].distributions[workload]
            candidate_distribution = candidate.latency[run_id].distributions[workload]
            for metric in METRICS:
                baseline_value = baseline_distribution.metrics[metric]
                candidate_value = candidate_distribution.metrics[metric]
                gate = "informational"
                limit = "-"
                result = "INFO"
                if workload == "aggregate" and metric == "p50_ns":
                    gate = "candidate_aggregate_p50_ceiling"
                    limit = str(P50_LIMIT_NS)
                    result = "PASS" if candidate_value <= P50_LIMIT_NS else "FAIL"
                elif metric in ("p99_ns", "p99_9_ns"):
                    gate = "candidate_each_run_le_baseline_worst_all_runs"
                    limit_value = tail_limits[workload][metric]
                    limit = str(limit_value)
                    result = "PASS" if candidate_value <= limit_value else "FAIL"
                if result == "FAIL":
                    passed = False
                print(
                    "\t".join(
                        (
                            run_id,
                            workload,
                            str(candidate_distribution.sample_count),
                            metric,
                            str(baseline_value),
                            str(candidate_value),
                            ratio(candidate_value, baseline_value),
                            str(candidate_value - baseline_value),
                            gate,
                            limit,
                            result,
                        )
                    )
                )
    print(
        "identity\tsemantic_digest_schema\t"
        + candidate.correctness.main["semantic_mutation_digest_schema"]
    )
    print(
        "identity\tsemantic_mutation_digest\t"
        + candidate.correctness.main["semantic_mutation_digest"]
    )
    print(
        "identity\tcapacity_profile_name\t"
        + candidate.preflight["capacity_profile_name"]
    )
    print(
        "identity\tcapacity_evidence_schema\t"
        + candidate.preflight["capacity_evidence_schema"]
    )
    print(
        "identity\tcapacity_evidence_sha256\t"
        + candidate.preflight["capacity_evidence_sha256"]
    )
    print("comparison\tresult\t" + ("PASS" if passed else "FAIL"))
    return passed


def sha256_argument(value: str) -> str:
    normalized = value.lower()
    if HEX64_RE.fullmatch(normalized) is None:
        raise argparse.ArgumentTypeError("must be a full 64-character SHA-256")
    return normalized


def commit_argument(value: str) -> str:
    normalized = value.lower()
    if COMMIT_RE.fullmatch(normalized) is None:
        raise argparse.ArgumentTypeError("must be a full 40- or 64-character commit ID")
    return normalized


def branch_argument(value: str) -> str:
    if SAFE_BRANCH_RE.fullmatch(value) is None or "//" in value or "/../" in f"/{value}/":
        raise argparse.ArgumentTypeError("must be a canonical branch name")
    return value


def positive_uint_argument(value: str) -> int:
    if UINT_RE.fullmatch(value) is None or int(value) == 0:
        raise argparse.ArgumentTypeError("must be a positive canonical unsigned integer")
    return int(value)


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Verify and compare one clean branch-5 artifact against one clean branch-6 "
            "artifact. Artifacts must come from the same host/boot/CPU/NUMA/THP/build "
            "protocol and contain matching sets of at least five full PASS runs."
        )
    )
    parser.add_argument("--baseline-dir", required=True, type=Path)
    parser.add_argument("--candidate-dir", required=True, type=Path)
    parser.add_argument("--expect-baseline-branch", required=True, type=branch_argument)
    parser.add_argument("--expect-baseline-commit", required=True, type=commit_argument)
    parser.add_argument("--expect-candidate-branch", required=True, type=branch_argument)
    parser.add_argument("--expect-candidate-commit", required=True, type=commit_argument)
    parser.add_argument(
        "--expect-candidate-capacity-evidence-sha256",
        required=True,
        type=sha256_argument,
    )
    parser.add_argument("--expect-trace-sha256", required=True, type=sha256_argument)
    parser.add_argument("--expect-records", required=True, type=positive_uint_argument)
    parser.add_argument("--expect-bytes", required=True, type=positive_uint_argument)
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    arguments = parse_arguments(argv)
    baseline_root = arguments.baseline_dir.absolute()
    candidate_root = arguments.candidate_dir.absolute()
    if baseline_root == candidate_root:
        raise ValidationError("baseline and candidate artifact directories are identical")
    if arguments.expect_baseline_branch == arguments.expect_candidate_branch:
        raise ValidationError("baseline and candidate expected branch names are identical")
    if arguments.expect_baseline_commit == arguments.expect_candidate_commit:
        raise ValidationError("baseline and candidate expected commits are identical")

    with tempfile.TemporaryDirectory(
        prefix="astra-order-book-comparator-snapshot-"
    ) as snapshot_directory:
        snapshot_root = Path(snapshot_directory)
        baseline_snapshot = snapshot_root / "baseline"
        candidate_snapshot = snapshot_root / "candidate"
        snapshot_artifact(baseline_root, baseline_snapshot, "baseline")
        snapshot_artifact(candidate_root, candidate_snapshot, "candidate")

        baseline = load_artifact(
            baseline_snapshot,
            "baseline",
            arguments.expect_baseline_branch,
            arguments.expect_baseline_commit,
        )
        candidate = load_artifact(
            candidate_snapshot,
            "candidate",
            arguments.expect_candidate_branch,
            arguments.expect_candidate_commit,
        )
        compare_identity(
            baseline,
            candidate,
            arguments.expect_trace_sha256,
            arguments.expect_candidate_capacity_evidence_sha256,
            arguments.expect_records,
            arguments.expect_bytes,
        )
        return 0 if emit_comparison(baseline, candidate) else 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ValidationError as exc:
        print(f"acceptance comparator: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
