#!/usr/bin/env python3

import hashlib
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def event(code: str, locate: int = 0, timestamp: int = 1) -> bytes:
    return (
        b"S"
        + struct.pack(">HH", locate, 1)
        + timestamp.to_bytes(6, "big")
        + code.encode("ascii")
    )


def record(message: bytes) -> bytes:
    return struct.pack(">H", len(message)) + message


def direct_listing() -> bytes:
    return b"O" + bytes(47)


def stock_directory(locate: int, ticker: bytes) -> bytes:
    message = bytearray(39)
    message[0] = ord("R")
    struct.pack_into(">H", message, 1, locate)
    message[11:19] = ticker.ljust(8, b" ")[:8]
    return bytes(message)


def add_order(locate: int, order_ref: int, ticker: bytes,
              quantity: int = 100, price: int = 1_000_000) -> bytes:
    message = bytearray(36)
    message[0] = ord("A")
    struct.pack_into(">H", message, 1, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    message[19] = ord("B")
    struct.pack_into(">I", message, 20, quantity)
    message[24:32] = ticker.ljust(8, b" ")[:8]
    struct.pack_into(">I", message, 32, price)
    return bytes(message)


def delete_order(locate: int, order_ref: int) -> bytes:
    message = bytearray(19)
    message[0] = ord("D")
    struct.pack_into(">H", message, 1, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    return bytes(message)


def cancel_order(locate: int, order_ref: int, quantity: int) -> bytes:
    message = bytearray(23)
    message[0] = ord("X")
    struct.pack_into(">H", message, 1, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    struct.pack_into(">I", message, 19, quantity)
    return bytes(message)


def broken_trade(match_number: int) -> bytes:
    message = bytearray(19)
    message[0] = ord("B")
    struct.pack_into(">Q", message, 11, match_number)
    return bytes(message)


def invoke(binary: str, path: Path, *arguments: str,
           expect_success: bool):
    result = subprocess.run(
        [binary, str(path), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if (result.returncode == 0) != expect_success:
        raise AssertionError(
            f"unexpected exit {result.returncode}\nstdout:\n{result.stdout}"
            f"\nstderr:\n{result.stderr}"
        )
    return result


def profile_fields(output: str):
    lines = [
        line for line in output.splitlines()
        if line.startswith("itch_trace_profile ")
    ]
    if len(lines) != 1:
        raise AssertionError(f"expected one profile record, got {lines}")
    return dict(
        re.findall(r"(?:^| )([A-Za-z0-9_]+)=(\"[^\"]*\"|\S+)", lines[0])
    )


def record_fields(output: str, prefix: str):
    lines = [
        line for line in output.splitlines()
        if line.startswith(prefix + " ")
    ]
    if len(lines) != 1:
        raise AssertionError(f"expected one {prefix} record, got {lines}")
    return dict(
        re.findall(r"(?:^| )([A-Za-z0-9_]+)=(\"[^\"]*\"|\S+)", lines[0])
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: ItchTraceProfileBinaryFileTest.py <trace-profile>"
        )
    binary = sys.argv[1]
    profiler_sha256 = hashlib.sha256(Path(binary).read_bytes()).hexdigest()
    payload = b"".join(
        record(message)
        for message in (
            event("O"),
            direct_listing(),
            event("S"),
            event("Q"),
            event("M"),
            event("E"),
            event("C"),
        )
    )

    with tempfile.TemporaryDirectory(
        prefix="astra-trace-profile-binaryfile-"
    ) as temp:
        directory = Path(temp)

        terminated_path = directory / "terminated.itch"
        terminated_path.write_bytes(payload + b"\0\0")
        terminated = invoke(
            binary, terminated_path, expect_success=True
        )
        fields = profile_fields(terminated.stdout)
        assert fields["records"] == "7"
        assert fields["bytes"] == str(len(payload) + 2)
        assert fields["zero_length_records"] == "1"
        assert fields["malformed_records"] == "0"
        assert fields["binaryfile_completion"] == "terminator"
        assert fields["binaryfile_sha256"] == hashlib.sha256(
            terminated_path.read_bytes()
        ).hexdigest()
        assert fields["profiler_sha256"] == profiler_sha256
        assert record_fields(
            terminated.stdout, "certification"
        )["semantic_clean"] == "1"
        storage = record_fields(
            terminated.stdout, "price_storage_logical_model"
        )
        assert storage["basis"] == "observed_pages_no_headroom"
        assert storage["registered_books"] == "0"
        assert storage["fixed_root_handle_bytes"] == str((1 << 32) * 4)
        assert storage["fixed_prepared_book_bytes"] == str(1 << 16)
        assert storage["fixed_book_summary_bytes"] == str((1 << 16) * 32)
        assert storage["fixed_book_occupancy_bytes"] == str(
            (1 << 16) * 2 * 8384
        )
        assert storage["page_capacity"] == "0"
        assert storage["page_owner_bytes"] == "8"
        assert storage["page_summary_bytes"] == "16"
        assert storage["page_occupancy_bytes"] == str(2 * 8384)
        modeled_components = (
            "page_arena_bytes",
            "page_owner_bytes",
            "page_summary_bytes",
            "page_occupancy_bytes",
            "fixed_root_handle_bytes",
            "fixed_prepared_book_bytes",
            "fixed_book_summary_bytes",
            "fixed_book_occupancy_bytes",
        )
        assert int(storage["total_logical_bytes"]) == sum(
            int(storage[field]) for field in modeled_components
        )

        clean_book_payload = b"".join(
            record(message)
            for message in (
                event("O"),
                stock_directory(7, b"AAPL"),
                event("S"),
                event("Q"),
                event("M"),
                add_order(7, 101, b"AAPL"),
                event("E"),
                broken_trade(5001),
                cancel_order(7, 101, 25),
                delete_order(7, 101),
                event("C"),
            )
        )
        clean_book_path = directory / "clean-book.itch"
        clean_book_path.write_bytes(clean_book_payload + b"\0\0")
        clean_book = invoke(
            binary, clean_book_path, expect_success=True
        )
        assert record_fields(
            clean_book.stdout, "certification"
        )["semantic_clean"] == "1"
        assert record_fields(
            clean_book.stdout, "orders"
        )["live_final"] == "0"
        lifecycle = record_fields(clean_book.stdout, "lifecycle")
        assert lifecycle["cancels_after_system_event_e"] == "1"
        assert lifecycle["deletes_after_system_event_e"] == "1"
        assert lifecycle["broken_trades_after_system_event_e"] == "1"
        assert lifecycle["invalid_messages_after_system_event_e"] == "0"

        invalid_post_e_payload = b"".join(
            record(message)
            for message in (
                event("O"),
                stock_directory(7, b"AAPL"),
                event("S"),
                event("Q"),
                event("M"),
                add_order(7, 101, b"AAPL"),
                event("E"),
                add_order(7, 102, b"AAPL", price=1_000_100),
                delete_order(7, 101),
                delete_order(7, 102),
                event("C"),
            )
        )
        invalid_post_e_path = directory / "invalid-post-e.itch"
        invalid_post_e_path.write_bytes(invalid_post_e_payload + b"\0\0")
        invalid_post_e = invoke(
            binary, invalid_post_e_path, expect_success=False
        )
        invalid_lifecycle = record_fields(
            invalid_post_e.stdout, "lifecycle"
        )
        assert invalid_lifecycle[
            "invalid_messages_after_system_event_e"
        ] == "1"
        assert record_fields(
            invalid_post_e.stdout, "orders"
        )["live_final"] == "0"
        assert record_fields(
            invalid_post_e.stdout, "certification"
        )["semantic_clean"] == "0"

        invalid_timestamp_payload = b"".join(
            record(message)
            for message in (
                event("O"),
                event("S"),
                event("Q", timestamp=86_400_000_000_000),
                event("M"),
                event("E"),
                event("C"),
            )
        )
        invalid_timestamp_path = directory / "invalid-timestamp.itch"
        invalid_timestamp_path.write_bytes(
            invalid_timestamp_payload + b"\0\0"
        )
        invalid_timestamp = invoke(
            binary, invalid_timestamp_path, expect_success=False
        )
        assert record_fields(
            invalid_timestamp.stdout, "lifecycle"
        )["invalid_timestamps"] == "1"
        assert record_fields(
            invalid_timestamp.stdout, "certification"
        )["semantic_clean"] == "0"

        pre_system_book_payload = b"".join(
            record(message)
            for message in (
                event("O"),
                stock_directory(7, b"AAPL"),
                add_order(7, 101, b"AAPL"),
                event("S"),
                event("Q"),
                event("M"),
                event("E"),
                delete_order(7, 101),
                event("C"),
            )
        )
        pre_system_book_path = directory / "pre-system-book.itch"
        pre_system_book_path.write_bytes(pre_system_book_payload + b"\0\0")
        pre_system_book = invoke(
            binary, pre_system_book_path, expect_success=False
        )
        assert record_fields(
            pre_system_book.stdout, "lifecycle"
        )["book_messages_before_system_hours"] == "1"

        conflicting_directory_payload = b"".join(
            record(message)
            for message in (
                event("O"),
                stock_directory(7, b"AAPL"),
                stock_directory(7, b"MSFT"),
                event("S"),
                event("Q"),
                event("M"),
                event("E"),
                event("C"),
            )
        )
        conflicting_directory_path = directory / "conflicting-directory.itch"
        conflicting_directory_path.write_bytes(
            conflicting_directory_payload + b"\0\0"
        )
        conflicting_directory = invoke(
            binary, conflicting_directory_path, expect_success=False
        )
        assert record_fields(
            conflicting_directory.stdout, "lifecycle"
        )["conflicting_directory_identities"] == "1"

        reused_ref_payload = b"".join(
            record(message)
            for message in (
                event("O"),
                stock_directory(7, b"AAPL"),
                event("S"),
                event("Q"),
                event("M"),
                add_order(7, 101, b"AAPL"),
                delete_order(7, 101),
                add_order(7, 101, b"AAPL", price=1_000_100),
                event("E"),
                event("C"),
            )
        )
        reused_ref_path = directory / "reused-order-ref.itch"
        reused_ref_path.write_bytes(reused_ref_payload + b"\0\0")
        reused_ref = invoke(
            binary, reused_ref_path, expect_success=False
        )
        reused_orders = record_fields(reused_ref.stdout, "orders")
        assert reused_orders["duplicates"] == "1"
        assert reused_orders["live_final"] == "0"
        assert record_fields(
            reused_ref.stdout, "certification"
        )["semantic_clean"] == "0"

        live_book_payload = clean_book_payload.replace(
            record(delete_order(7, 101)), b"", 1
        )
        live_book_path = directory / "live-book.itch"
        live_book_path.write_bytes(live_book_payload + b"\0\0")
        live_book = invoke(
            binary, live_book_path, expect_success=False
        )
        assert "semantic ITCH reconstruction gate failed" in live_book.stderr
        assert record_fields(
            live_book.stdout, "certification"
        )["semantic_clean"] == "0"
        assert record_fields(
            live_book.stdout, "orders"
        )["live_final"] == "1"

        incomplete_path = directory / "incomplete.itch"
        incomplete_path.write_bytes(payload)
        incomplete = invoke(
            binary, incomplete_path, expect_success=False
        )
        assert (
            "physical EOF before zero-length BinaryFILE terminator"
            in incomplete.stderr
        )
        assert (
            profile_fields(incomplete.stdout)["binaryfile_completion"]
            == "incomplete_eof"
        )

        legacy = invoke(
            binary,
            incomplete_path,
            "--allow-legacy-eof-after-sc",
            expect_success=True,
        )
        fields = profile_fields(legacy.stdout)
        assert fields["records"] == "7"
        assert fields["bytes"] == str(len(payload))
        assert fields["zero_length_records"] == "0"
        assert fields["malformed_records"] == "0"
        assert fields["binaryfile_completion"] == "legacy_sc_eof"
        assert fields["binaryfile_sha256"] == hashlib.sha256(
            incomplete_path.read_bytes()
        ).hexdigest()
        assert fields["profiler_sha256"] == profiler_sha256

        trailing_path = directory / "trailing.itch"
        trailing_path.write_bytes(payload + b"\0\0\x7f")
        trailing = invoke(
            binary, trailing_path, expect_success=False
        )
        assert (
            "data follows zero-length BinaryFILE terminator"
            in trailing.stderr
        )
        assert (
            profile_fields(trailing.stdout)["binaryfile_completion"]
            == "trailing_data"
        )
        assert profile_fields(trailing.stdout)[
            "binaryfile_sha256"
        ] == hashlib.sha256(trailing_path.read_bytes()).hexdigest()

        missing_so_path = directory / "missing-so.itch"
        missing_so_path.write_bytes(record(event("C")) + b"\0\0")
        missing_so = invoke(
            binary, missing_so_path, expect_success=False
        )
        assert "first ITCH record is not System Event O" in missing_so.stderr
        assert (
            profile_fields(missing_so.stdout)["binaryfile_completion"]
            == "missing_so"
        )

        nonzero_system_locate_path = directory / "system-locate.itch"
        nonzero_system_locate_path.write_bytes(
            record(event("O", locate=7)) + b"\0\0"
        )
        nonzero_system_locate = invoke(
            binary, nonzero_system_locate_path, expect_success=False
        )
        assert (
            "System Event stock locate is not zero"
            in nonzero_system_locate.stderr
        )
        assert (
            profile_fields(
                nonzero_system_locate.stdout
            )["binaryfile_completion"]
            == "malformed_record"
        )

        terminator_before_sc_path = directory / "terminator-before-sc.itch"
        terminator_before_sc_path.write_bytes(
            record(event("O")) + b"\0\0"
        )
        terminator_before_sc = invoke(
            binary, terminator_before_sc_path, expect_success=False
        )
        assert (
            "BinaryFILE terminator before ITCH System Event C"
            in terminator_before_sc.stderr
        )
        assert (
            profile_fields(
                terminator_before_sc.stdout
            )["binaryfile_completion"]
            == "terminator_before_sc"
        )

        record_after_sc_path = directory / "record-after-sc.itch"
        record_after_sc_path.write_bytes(
            payload
            + record(event("H"))
            + b"\0\0"
        )
        record_after_sc = invoke(
            binary, record_after_sc_path, expect_success=False
        )
        assert "ITCH record follows System Event C" in record_after_sc.stderr
        assert (
            profile_fields(record_after_sc.stdout)["binaryfile_completion"]
            == "record_after_sc"
        )

        unsupported_path = directory / "unsupported.itch"
        unsupported_path.write_bytes(
            record(event("O")) + record(b"Z" + bytes(11)) + b"\0\0"
        )
        unsupported = invoke(
            binary, unsupported_path, expect_success=False
        )
        assert (
            "unsupported ITCH type or invalid message length"
            in unsupported.stderr
        )
        assert (
            profile_fields(unsupported.stdout)["binaryfile_completion"]
            == "malformed_record"
        )
        assert profile_fields(unsupported.stdout)[
            "binaryfile_sha256"
        ] == hashlib.sha256(unsupported_path.read_bytes()).hexdigest()

        wrong_o_length_path = directory / "wrong-o-length.itch"
        wrong_o_length_path.write_bytes(
            record(event("O")) + record(b"O" + bytes(46)) + b"\0\0"
        )
        wrong_o_length = invoke(
            binary, wrong_o_length_path, expect_success=False
        )
        assert (
            "unsupported ITCH type or invalid message length"
            in wrong_o_length.stderr
        )
        assert (
            profile_fields(wrong_o_length.stdout)["binaryfile_completion"]
            == "malformed_record"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
