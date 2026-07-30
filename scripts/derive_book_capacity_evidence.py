#!/usr/bin/env python3

"""Derive one canonical custom book-capacity evidence manifest.

The observed values come only from a completed, semantic-clean
astra_itch_trace_profile report. Capacity beyond those observations is
operator policy and must be supplied as explicit absolute headroom.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import stat
import subprocess
import sys
from pathlib import Path


UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
MAX_PRICE_PAGE_CAPACITY = UINT32_MAX - 1
PROFILE_TOKEN = re.compile(r"^[A-Za-z0-9_.:/+-]{1,128}$")
LOWER_SHA256 = re.compile(r"^[0-9a-f]{64}$")

ZERO_LIFECYCLE_FIELDS = (
    "conflicting_directory_after_system_hours",
    "conflicting_directory_identities",
    "invalid_directory_identities",
    "duplicate_directory_tickers",
    "invalid_timestamps",
    "order_adds_before_directory",
    "order_add_stock_mismatches",
    "book_messages_before_system_hours",
    "system_event_sequence_errors",
    "unsupported_system_event_codes",
    "invalid_messages_after_system_event_e",
)

ZERO_ORDER_FIELDS = (
    "live_final",
    "duplicates",
    "missing_D",
    "missing_X",
    "missing_EC",
    "missing_U",
    "same_ref_U",
    "over_X",
    "over_EC",
    "zero_qty",
    "invalid_sides",
    "locate_mismatches",
    "invariant_failures",
)

ZERO_PRICE_FIELDS = (
    "active_levels_final",
    "active_price_pages_final",
)


class DerivationError(ValueError):
    pass


def regular_file(path: Path, label: str) -> None:
    try:
        mode = path.stat().st_mode
    except OSError as error:
        raise DerivationError(f"cannot stat {label}: {path}: {error}") from error
    if not stat.S_ISREG(mode):
        raise DerivationError(f"{label} is not a regular file: {path}")


def sha256_file(path: Path, label: str) -> tuple[str, int]:
    regular_file(path, label)
    digest = hashlib.sha256()
    total = 0
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
                total += len(chunk)
    except OSError as error:
        raise DerivationError(f"cannot read {label}: {path}: {error}") from error
    if total == 0:
        raise DerivationError(f"{label} is empty: {path}")
    return digest.hexdigest(), total


def verify_profiler_reexecution(
    profiler: Path,
    binaryfile: Path,
    profile_contents: str,
    corpus_contents: str,
) -> None:
    if not os.access(profiler, os.X_OK):
        raise DerivationError(
            f"profiler binary is not executable: {profiler}"
        )

    profile = profile_record(profile_contents, "itch_trace_profile")
    corpus = key_value_manifest(corpus_contents)
    input_label = profile.get("path", "")
    if not input_label or corpus.get("binaryfile_path") != input_label:
        raise DerivationError(
            "profile path differs from corpus manifest binaryfile_path"
        )
    try:
        if not os.path.samefile(input_label, binaryfile):
            raise DerivationError(
                "profile input path does not identify the supplied BinaryFILE"
            )
    except OSError as error:
        raise DerivationError(
            "cannot resolve the profile input path against the supplied "
            f"BinaryFILE: {error}"
        ) from error

    terminator = corpus.get("zero_length_terminator_present")
    if terminator not in ("true", "false"):
        raise DerivationError(
            "corpus manifest zero_length_terminator_present must be true or false"
        )
    command = [str(profiler.resolve(strict=True)), input_label]
    if terminator == "false":
        command.append("--allow-legacy-eof-after-sc")

    try:
        reproduced = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise DerivationError(
            f"cannot execute frozen profiler binary: {error}"
        ) from error
    if reproduced.returncode != 0:
        detail = reproduced.stderr[:4096].decode("utf-8", errors="replace")
        raise DerivationError(
            "frozen profiler re-execution failed"
            + (f": {detail.strip()}" if detail.strip() else "")
        )
    try:
        reproduced_profile = reproduced.stdout.decode("ascii")
    except UnicodeDecodeError as error:
        raise DerivationError(
            "frozen profiler re-execution produced non-ASCII stdout"
        ) from error
    if reproduced_profile != profile_contents:
        raise DerivationError(
            "supplied profile output does not exactly match frozen profiler "
            "re-execution"
        )


def read_small_ascii(path: Path, label: str, maximum_bytes: int) -> str:
    regular_file(path, label)
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise DerivationError(f"cannot read {label}: {path}: {error}") from error
    if not payload:
        raise DerivationError(f"{label} is empty: {path}")
    if len(payload) > maximum_bytes:
        raise DerivationError(
            f"{label} exceeds the {maximum_bytes}-byte safety limit: {path}"
        )
    try:
        return payload.decode("ascii")
    except UnicodeDecodeError as error:
        raise DerivationError(f"{label} is not ASCII: {path}") from error


def canonical_uint(value: str, label: str, maximum: int = UINT64_MAX) -> int:
    if not value or (len(value) > 1 and value[0] == "0") or not value.isascii():
        raise DerivationError(f"{label} is not a canonical unsigned integer")
    if not value.isdecimal():
        raise DerivationError(f"{label} is not a canonical unsigned integer")
    parsed = int(value, 10)
    if parsed > maximum:
        raise DerivationError(f"{label} exceeds {maximum}")
    return parsed


def positive_argument(value: str) -> int:
    try:
        parsed = canonical_uint(value, "argument")
    except DerivationError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if parsed == 0:
        raise argparse.ArgumentTypeError("argument must be positive")
    return parsed


def key_value_manifest(contents: str) -> dict[str, str]:
    if not contents.endswith("\n") or contents.endswith("\n\n"):
        raise DerivationError(
            "corpus manifest must have exactly one final LF"
        )
    if "\r" in contents or "\0" in contents:
        raise DerivationError(
            "corpus manifest must use canonical ASCII LF line endings"
        )
    result: dict[str, str] = {}
    for line in contents[:-1].split("\n"):
        if "=" not in line:
            raise DerivationError("corpus manifest has a non-key/value line")
        key, value = line.split("=", 1)
        if not key or not value or key in result:
            raise DerivationError(
                "corpus manifest has an empty or duplicate key/value"
            )
        result[key] = value
    return result


def profile_record(contents: str, record_name: str) -> dict[str, str]:
    matching = [
        line
        for line in contents.splitlines()
        if line.startswith(record_name + " ")
    ]
    if len(matching) != 1:
        raise DerivationError(
            f"profile must contain exactly one {record_name} record"
        )
    try:
        tokens = shlex.split(matching[0], posix=True)
    except ValueError as error:
        raise DerivationError(
            f"profile {record_name} record is not valid shell-style text"
        ) from error
    if not tokens or tokens[0] != record_name:
        raise DerivationError(f"profile {record_name} record is malformed")

    fields: dict[str, str] = {}
    for token in tokens[1:]:
        if "=" not in token:
            raise DerivationError(
                f"profile {record_name} contains a non-key/value token"
            )
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise DerivationError(
                f"profile {record_name} has an empty or duplicate field"
            )
        fields[key] = value
    return fields


def field_uint(
    record_name: str,
    fields: dict[str, str],
    key: str,
    maximum: int = UINT64_MAX,
) -> int:
    try:
        value = fields[key]
    except KeyError as error:
        raise DerivationError(
            f"profile {record_name} is missing {key}"
        ) from error
    return canonical_uint(value, f"profile {record_name}.{key}", maximum)


def require_zero_fields(
    record_name: str, fields: dict[str, str], keys: tuple[str, ...]
) -> None:
    for key in keys:
        if field_uint(record_name, fields, key) != 0:
            raise DerivationError(
                f"profile is not semantic-clean: {record_name}.{key} is nonzero"
            )


def validate_corpus_manifest(
    fields: dict[str, str],
    profile: dict[str, str],
    completion: str,
    profile_bytes: int,
    actual_binaryfile_sha256: str,
    actual_binaryfile_bytes: int,
) -> None:
    if fields.get("schema") != "astra_itch_trace_manifest_v1":
        raise DerivationError(
            "unsupported corpus manifest schema; expected "
            "astra_itch_trace_manifest_v1"
        )
    if fields.get("first_itch_message") != "system_event_O":
        raise DerivationError(
            "corpus manifest does not certify System Event O first"
        )
    if fields.get("final_itch_message") != "system_event_C":
        raise DerivationError(
            "corpus manifest does not certify System Event C final"
        )
    manifest_binaryfile_sha256 = fields.get("binaryfile_sha256", "")
    if not LOWER_SHA256.fullmatch(manifest_binaryfile_sha256):
        raise DerivationError(
            "corpus manifest binaryfile_sha256 is not lowercase SHA-256"
        )
    manifest_bytes = canonical_uint(
        fields.get("binaryfile_bytes", ""),
        "corpus manifest binaryfile_bytes",
    )
    if manifest_bytes != profile_bytes:
        raise DerivationError(
            "profile byte count differs from corpus manifest binaryfile_bytes"
        )
    if actual_binaryfile_bytes != manifest_bytes:
        raise DerivationError(
            "actual BinaryFILE byte count differs from corpus manifest"
        )
    if actual_binaryfile_sha256 != manifest_binaryfile_sha256:
        raise DerivationError(
            "actual BinaryFILE SHA-256 differs from corpus manifest"
        )
    manifest_path = fields.get("binaryfile_path")
    if not manifest_path or manifest_path != profile.get("path"):
        raise DerivationError(
            "profile path differs from corpus manifest binaryfile_path"
        )

    terminator = fields.get("zero_length_terminator_present")
    expected_terminator = completion == "terminator"
    if terminator not in ("true", "false"):
        raise DerivationError(
            "corpus manifest zero_length_terminator_present must be true or false"
        )
    if (terminator == "true") != expected_terminator:
        raise DerivationError(
            "profile completion differs from corpus manifest terminator policy"
        )


def derive(
    profile_contents: str,
    corpus_contents: str,
    profile_name: str,
    direct_headroom: int,
    price_headroom: int,
    fallback_buckets: int,
    actual_binaryfile_sha256: str,
    actual_binaryfile_bytes: int,
    actual_profiler_sha256: str,
) -> tuple[int, int, int, int]:
    if not PROFILE_TOKEN.fullmatch(profile_name):
        raise DerivationError(
            "profile name must be a 1-128 character audit-safe token"
        )
    if fallback_buckets > UINT32_MAX or (
        fallback_buckets & (fallback_buckets - 1)
    ):
        raise DerivationError(
            "order fallback buckets must be a uint32 power of two"
        )
    if direct_headroom > UINT64_MAX:
        raise DerivationError("direct-order headroom exceeds uint64")
    if price_headroom > UINT32_MAX:
        raise DerivationError("price-page headroom exceeds uint32")

    profile = profile_record(profile_contents, "itch_trace_profile")
    lifecycle = profile_record(profile_contents, "lifecycle")
    orders = profile_record(profile_contents, "orders")
    prices = profile_record(profile_contents, "prices")
    certification = profile_record(profile_contents, "certification")

    if certification.get("semantic_clean") != "1":
        raise DerivationError("profile certification semantic_clean is not 1")
    if field_uint("itch_trace_profile", profile, "records") == 0:
        raise DerivationError("profile contains no ITCH records")
    profile_bytes = field_uint("itch_trace_profile", profile, "bytes")
    if profile_bytes == 0:
        raise DerivationError("profile contains no BinaryFILE bytes")
    if field_uint("itch_trace_profile", profile, "malformed_records") != 0:
        raise DerivationError("profile contains malformed records")
    if field_uint(
        "itch_trace_profile", profile, "prices_above_documented_max"
    ) != 0:
        raise DerivationError("profile contains prices above the ITCH maximum")
    profiled_binaryfile_sha256 = profile.get("binaryfile_sha256", "")
    if not LOWER_SHA256.fullmatch(profiled_binaryfile_sha256):
        raise DerivationError(
            "profile BinaryFILE SHA-256 is not 64 lowercase hex digits"
        )
    if profiled_binaryfile_sha256 != actual_binaryfile_sha256:
        raise DerivationError(
            "profile BinaryFILE SHA-256 differs from actual BinaryFILE"
        )
    profiled_profiler_sha256 = profile.get("profiler_sha256", "")
    if not LOWER_SHA256.fullmatch(profiled_profiler_sha256):
        raise DerivationError(
            "profile profiler SHA-256 is not 64 lowercase hex digits"
        )
    if profiled_profiler_sha256 != actual_profiler_sha256:
        raise DerivationError(
            "profile profiler SHA-256 differs from actual profiler binary"
        )

    completion = profile.get("binaryfile_completion")
    if completion not in ("terminator", "legacy_sc_eof"):
        raise DerivationError(
            "profile BinaryFILE completion is not terminator or legacy_sc_eof"
        )
    zero_records = field_uint(
        "itch_trace_profile", profile, "zero_length_records"
    )
    if (completion == "terminator" and zero_records != 1) or (
        completion == "legacy_sc_eof" and zero_records != 0
    ):
        raise DerivationError(
            "profile zero-length record count disagrees with completion"
        )

    require_zero_fields("lifecycle", lifecycle, ZERO_LIFECYCLE_FIELDS)
    if field_uint("lifecycle", lifecycle, "daily_system_events_seen") != 6:
        raise DerivationError(
            "profile does not contain the six ordered daily System Events"
        )
    if field_uint("lifecycle", lifecycle, "system_event_e") != 1:
        raise DerivationError(
            "profile does not contain exactly one System Event E"
        )

    require_zero_fields("orders", orders, ZERO_ORDER_FIELDS)
    if field_uint("orders", orders, "new_refs") == 0:
        raise DerivationError(
            "profile has no displayed orders from which to size the order table"
        )
    maximum_order_ref = field_uint(
        "itch_trace_profile", profile, "max_order_ref"
    )
    maximum_new_order_ref = field_uint("orders", orders, "id_max")
    if maximum_order_ref == 0 or maximum_order_ref != maximum_new_order_ref:
        raise DerivationError(
            "profile max_order_ref and orders.id_max are inconsistent"
        )

    require_zero_fields("prices", prices, ZERO_PRICE_FIELDS)
    unique_price_pages = field_uint(
        "prices", prices, "ever_price_pages", UINT32_MAX
    )
    if unique_price_pages == 0:
        raise DerivationError(
            "profile has no lifetime price pages from which to size the arena"
        )
    distinct_pages = field_uint(
        "itch_trace_profile",
        profile,
        "distinct_locate_price_pages",
        UINT32_MAX,
    )
    if unique_price_pages > distinct_pages:
        raise DerivationError(
            "profile lifetime price pages exceed distinct observed price pages"
        )

    corpus = key_value_manifest(corpus_contents)
    validate_corpus_manifest(
        corpus,
        profile,
        completion,
        profile_bytes,
        actual_binaryfile_sha256,
        actual_binaryfile_bytes,
    )

    direct_slots = maximum_order_ref + 1 + direct_headroom
    if direct_slots > UINT64_MAX:
        raise DerivationError(
            "observed maximum plus direct-order headroom exceeds uint64"
        )
    price_page_capacity = unique_price_pages + price_headroom
    if price_page_capacity > MAX_PRICE_PAGE_CAPACITY:
        raise DerivationError(
            "observed pages plus price-page headroom exceed loader capacity"
        )
    return (
        direct_slots,
        price_page_capacity,
        maximum_order_ref,
        unique_price_pages,
    )


def evidence_bytes(
    profile_name: str,
    corpus_sha256: str,
    profiler_sha256: str,
    profile_output_sha256: str,
    direct_slots: int,
    fallback_buckets: int,
    price_page_capacity: int,
    maximum_order_ref: int,
    unique_price_pages: int,
    direct_headroom: int,
    price_headroom: int,
) -> bytes:
    return (
        "schema=astra_book_capacity_evidence_v2\n"
        f"profile_name={profile_name}\n"
        f"corpus_manifest_sha256={corpus_sha256}\n"
        f"profiler_sha256={profiler_sha256}\n"
        f"profile_output_sha256={profile_output_sha256}\n"
        f"order_direct_slots={direct_slots}\n"
        f"order_fallback_buckets={fallback_buckets}\n"
        f"price_page_capacity={price_page_capacity}\n"
        f"profiled_max_order_ref={maximum_order_ref}\n"
        f"profiled_unique_price_pages={unique_price_pages}\n"
        f"minimum_direct_order_headroom={direct_headroom}\n"
        f"minimum_price_page_headroom={price_headroom}\n"
    ).encode("ascii")


def write_idempotent(path: Path, payload: bytes) -> str:
    try:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644
        )
    except FileExistsError:
        try:
            existing = path.read_bytes()
        except OSError as error:
            raise DerivationError(
                f"cannot inspect existing output: {path}: {error}"
            ) from error
        if existing != payload:
            raise DerivationError(
                f"refusing to replace different existing output: {path}"
            )
        return "unchanged"
    except OSError as error:
        raise DerivationError(f"cannot create output: {path}: {error}") from error

    try:
        with os.fdopen(descriptor, "wb") as destination:
            destination.write(payload)
            destination.flush()
            os.fsync(destination.fileno())
    except BaseException:
        try:
            path.unlink()
        except OSError:
            pass
        raise
    return "created"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "derive a canonical custom capacity manifest from one "
            "semantic-clean ITCH trace profile"
        )
    )
    parser.add_argument(
        "--profile-output",
        required=True,
        type=Path,
        help="complete stdout captured from astra_itch_trace_profile",
    )
    parser.add_argument(
        "--corpus-manifest",
        required=True,
        type=Path,
        help="canonical astra_itch_trace_manifest_v1 file for that trace",
    )
    parser.add_argument(
        "--binaryfile",
        required=True,
        type=Path,
        help="actual BinaryFILE whose size and SHA-256 must match the manifest",
    )
    parser.add_argument(
        "--profiler-binary",
        required=True,
        type=Path,
        help="unchanged profiler executable that produced the profile",
    )
    parser.add_argument(
        "--profile-name",
        required=True,
        help="new audit-safe custom deployment profile token",
    )
    parser.add_argument(
        "--minimum-direct-order-headroom",
        required=True,
        type=positive_argument,
        help="explicit absolute direct slots beyond observed max+1",
    )
    parser.add_argument(
        "--minimum-price-page-headroom",
        required=True,
        type=positive_argument,
        help="explicit absolute pages beyond observed lifetime pages",
    )
    parser.add_argument(
        "--order-fallback-buckets",
        required=True,
        type=positive_argument,
        help="explicit uint32 power-of-two anomaly fallback bucket count",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="new canonical evidence manifest (different existing file rejected)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        profile_contents = read_small_ascii(
            arguments.profile_output, "trace profile output", 64 * 1024 * 1024
        )
        profile_output_sha256 = hashlib.sha256(
            profile_contents.encode("ascii")
        ).hexdigest()
        corpus_contents = read_small_ascii(
            arguments.corpus_manifest, "corpus manifest", 1024 * 1024
        )
        # Hash the exact bytes validated below, avoiding a second-read race
        # between corpus validation and provenance binding.
        corpus_sha256 = hashlib.sha256(
            corpus_contents.encode("ascii")
        ).hexdigest()
        binaryfile_sha256, binaryfile_bytes = sha256_file(
            arguments.binaryfile, "BinaryFILE"
        )
        profiler_sha256, _ = sha256_file(
            arguments.profiler_binary, "profiler binary"
        )
        (
            direct_slots,
            price_page_capacity,
            maximum_order_ref,
            unique_price_pages,
        ) = derive(
            profile_contents,
            corpus_contents,
            arguments.profile_name,
            arguments.minimum_direct_order_headroom,
            arguments.minimum_price_page_headroom,
            arguments.order_fallback_buckets,
            binaryfile_sha256,
            binaryfile_bytes,
            profiler_sha256,
        )
        # A profile text file is not trusted merely because it names the
        # correct corpus and profiler hashes: its observed capacity values
        # could have been edited consistently. Re-run the exact frozen binary
        # over the exact input label and require byte-identical stdout before
        # emitting deployable evidence.
        verify_profiler_reexecution(
            arguments.profiler_binary,
            arguments.binaryfile,
            profile_contents,
            corpus_contents,
        )
        payload = evidence_bytes(
            arguments.profile_name,
            corpus_sha256,
            profiler_sha256,
            profile_output_sha256,
            direct_slots,
            arguments.order_fallback_buckets,
            price_page_capacity,
            maximum_order_ref,
            unique_price_pages,
            arguments.minimum_direct_order_headroom,
            arguments.minimum_price_page_headroom,
        )
        status = write_idempotent(arguments.output, payload)
        evidence_sha256 = hashlib.sha256(payload).hexdigest()
    except (DerivationError, OSError) as error:
        print(f"capacity evidence derivation failed: {error}", file=sys.stderr)
        return 1

    print(
        "capacity_evidence"
        f" status={status}"
        f" output={shlex.quote(str(arguments.output))}"
        f" sha256={evidence_sha256}"
        f" corpus_manifest_sha256={corpus_sha256}"
        f" binaryfile_sha256={binaryfile_sha256}"
        f" profiler_sha256={profiler_sha256}"
        f" profile_output_sha256={profile_output_sha256}"
        f" order_direct_slots={direct_slots}"
        f" order_fallback_buckets={arguments.order_fallback_buckets}"
        f" price_page_capacity={price_page_capacity}"
        f" profiled_max_order_ref={maximum_order_ref}"
        f" profiled_unique_price_pages={unique_price_pages}"
        f" minimum_direct_order_headroom="
        f"{arguments.minimum_direct_order_headroom}"
        f" minimum_price_page_headroom="
        f"{arguments.minimum_price_page_headroom}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
