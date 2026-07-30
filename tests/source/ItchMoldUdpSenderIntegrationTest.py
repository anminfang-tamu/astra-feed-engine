#!/usr/bin/env python3

from __future__ import annotations

import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


SESSION = b"TESTSESS01"


def fail(message: str) -> None:
    raise AssertionError(message)


def system_event(event_code: str, timestamp_ns: int) -> bytes:
    message = bytearray(12)
    message[0] = ord("S")
    message[5:11] = timestamp_ns.to_bytes(6, byteorder="big")
    message[11] = ord(event_code)
    return bytes(message)


def with_timestamp(message: bytes, timestamp_ns: int) -> bytes:
    stamped = bytearray(message)
    stamped[5:11] = timestamp_ns.to_bytes(6, byteorder="big")
    return bytes(stamped)


def itch_message(message_type: str, length: int, locate: int) -> bytearray:
    message = bytearray(length)
    message[0] = ord(message_type)
    struct.pack_into(">H", message, 1, locate)
    return message


def stock_directory(locate: int, ticker: str) -> bytes:
    message = itch_message("R", 39, locate)
    message[11:19] = ticker.encode("ascii").ljust(8, b" ")
    message[19] = ord("Q")
    message[20] = ord("N")
    struct.pack_into(">I", message, 21, 100)
    message[25] = ord("N")
    message[26] = ord("C")
    message[29] = ord("P")
    message[30] = ord("N")
    message[31] = ord("N")
    message[32] = ord("1")
    message[33] = ord("N")
    struct.pack_into(">I", message, 34, 1)
    message[38] = ord("N")
    return bytes(message)


def add_order(
    locate: int,
    order_ref: int,
    side: str,
    quantity: int,
    ticker: str,
    price: int,
    *,
    attributed: bool = False,
) -> bytes:
    message = itch_message("F" if attributed else "A",
                           40 if attributed else 36, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    message[19] = ord(side)
    struct.pack_into(">I", message, 20, quantity)
    message[24:32] = ticker.encode("ascii").ljust(8, b" ")
    struct.pack_into(">I", message, 32, price)
    if attributed:
        message[36:40] = b"TEST"
    return bytes(message)


def order_cancel(locate: int, order_ref: int, quantity: int) -> bytes:
    message = itch_message("X", 23, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    struct.pack_into(">I", message, 19, quantity)
    return bytes(message)


def order_execution(
    locate: int,
    order_ref: int,
    quantity: int,
    match_number: int,
    *,
    execution_price: int | None = None,
) -> bytes:
    message = itch_message("C" if execution_price is not None else "E",
                           36 if execution_price is not None else 31, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    struct.pack_into(">I", message, 19, quantity)
    struct.pack_into(">Q", message, 23, match_number)
    if execution_price is not None:
        message[31] = ord("Y")
        struct.pack_into(">I", message, 32, execution_price)
    return bytes(message)


def order_delete(locate: int, order_ref: int) -> bytes:
    message = itch_message("D", 19, locate)
    struct.pack_into(">Q", message, 11, order_ref)
    return bytes(message)


def order_replace(
    locate: int,
    old_order_ref: int,
    new_order_ref: int,
    quantity: int,
    price: int,
) -> bytes:
    message = itch_message("U", 35, locate)
    struct.pack_into(">Q", message, 11, old_order_ref)
    struct.pack_into(">Q", message, 19, new_order_ref)
    struct.pack_into(">I", message, 27, quantity)
    struct.pack_into(">I", message, 31, price)
    return bytes(message)


def lifecycle_crud_messages() -> list[bytes]:
    messages: list[bytes] = []

    def append(message: bytes) -> None:
        stamped = bytearray(message)
        stamped[5:11] = (len(messages) + 1).to_bytes(6, byteorder="big")
        messages.append(bytes(stamped))

    append(system_event("O", 0))
    append(stock_directory(1, "AAPL"))
    append(stock_directory(2, "MSFT"))
    append(system_event("S", 0))
    append(system_event("Q", 0))

    append(add_order(1, 101, "B", 100, "AAPL", 100_000))
    append(add_order(
        1, 102, "B", 40, "AAPL", 100_000, attributed=True
    ))
    append(add_order(1, 103, "S", 70, "AAPL", 101_000))
    append(add_order(1, 105, "B", 25, "AAPL", 98_000))
    append(add_order(2, 201, "S", 200, "MSFT", 500_000))
    append(add_order(
        2, 202, "S", 50, "MSFT", 500_000, attributed=True
    ))
    append(add_order(2, 203, "B", 80, "MSFT", 499_000))
    append(add_order(2, 205, "S", 20, "MSFT", 501_000))

    append(order_cancel(1, 101, 30))
    append(order_execution(1, 102, 10, 1_001))
    append(order_execution(
        1, 103, 20, 1_002, execution_price=101_100
    ))
    append(order_replace(1, 101, 104, 60, 99_000))
    append(order_delete(1, 102))

    append(order_cancel(2, 201, 20))
    append(order_execution(2, 202, 50, 2_001))
    append(order_execution(
        2, 201, 30, 2_002, execution_price=500_100
    ))
    append(order_replace(2, 203, 204, 90, 499_500))

    append(system_event("M", 0))
    append(system_event("E", 0))
    # The official 2026-06-12 Nasdaq archive contains valid partial X
    # mutations interleaved with D messages in its post-E teardown tail.
    append(order_delete(1, 103))
    append(order_cancel(2, 201, 10))
    for locate, order_ref in (
        (1, 104),
        (1, 105),
        (2, 201),
        (2, 204),
        (2, 205),
    ):
        append(order_delete(locate, order_ref))
    append(system_event("C", 0))
    return messages


def write_messages(path: Path, messages: list[bytes]) -> None:
    with path.open("wb") as trace:
        for message in messages:
            trace.write(struct.pack(">H", len(message)))
            trace.write(message)
        trace.write(b"\x00\x00")


def write_trace(
    path: Path,
    event_codes: tuple[str, ...] = ("O", "C"),
    terminator: bool = True,
) -> None:
    with path.open("wb") as trace:
        for timestamp_ns, event_code in enumerate(event_codes, start=1):
            message = system_event(event_code, timestamp_ns)
            trace.write(struct.pack(">H", len(message)))
            trace.write(message)
        if terminator:
            trace.write(b"\x00\x00")


def blank_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith("ASTRA_"):
            del environment[name]
    return environment


def clean_environment() -> dict[str, str]:
    environment = blank_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "2",
            "ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": "3",
            "ASTRA_EOS_INTERVAL_MS": "0",
            "ASTRA_BINARYFILE_COMPLETION": "strict",
        }
    )
    return environment


def command(binary: Path, trace: Path, port: str, *optional: str) -> list[str]:
    return [
        str(binary),
        str(trace),
        "127.0.0.1",
        port,
        *optional,
    ]


def receive_all(listener: socket.socket) -> list[bytes]:
    packets: list[bytes] = []
    listener.settimeout(0.1)
    while True:
        try:
            packet, _ = listener.recvfrom(4096)
        except socket.timeout:
            return packets
        packets.append(packet)


def run_and_receive_timed(
    arguments: list[str],
    environment: dict[str, str],
    listener: socket.socket,
    *,
    timeout: float,
) -> tuple[subprocess.CompletedProcess[str], list[tuple[bytes, float]]]:
    process = subprocess.Popen(
        arguments,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    timed_packets: list[tuple[bytes, float]] = []
    deadline = time.monotonic() + timeout
    listener.settimeout(0.05)

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            process.kill()
            stdout, stderr = process.communicate()
            fail(
                f"sender timed out after {timeout:.1f}s\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            )
        listener.settimeout(min(0.05, remaining))
        try:
            packet, _ = listener.recvfrom(4096)
        except socket.timeout:
            if process.poll() is not None:
                break
            continue
        timed_packets.append((packet, time.monotonic()))

    stdout, stderr = process.communicate(
        timeout=max(0.1, deadline - time.monotonic())
    )
    result = subprocess.CompletedProcess(
        process.args,
        process.returncode,
        stdout,
        stderr,
    )
    return result, timed_packets


def parse_stats(stdout: str) -> dict[str, str]:
    lines = [line for line in stdout.splitlines()
             if line.startswith("sender_stats ")]
    if len(lines) != 1:
        fail(f"expected one sender_stats line, got {lines!r}\n{stdout}")
    fields: dict[str, str] = {}
    for field in lines[0].split()[1:]:
        if "=" not in field:
            fail(f"malformed sender_stats field: {field!r}")
        key, value = field.split("=", 1)
        fields[key] = value
    return fields


def parse_record(stdout: str, record_name: str) -> dict[str, str]:
    prefix = record_name + " "
    lines = [line for line in stdout.splitlines()
             if line.startswith(prefix)]
    if len(lines) != 1:
        fail(f"expected one {record_name} line, got {lines!r}\n{stdout}")
    fields: dict[str, str] = {}
    for field in lines[0].split()[1:]:
        if "=" not in field:
            fail(f"malformed {record_name} field: {field!r}")
        key, value = field.split("=", 1)
        if key in fields:
            fail(f"duplicate {record_name} field: {key!r}")
        fields[key] = value
    return fields


def reserve_udp_ports(count: int) -> list[int]:
    reservations: list[socket.socket] = []
    try:
        for _ in range(count):
            reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            reservation.bind(("127.0.0.1", 0))
            reservations.append(reservation)
        return [reservation.getsockname()[1]
                for reservation in reservations]
    finally:
        for reservation in reservations:
            reservation.close()


def wait_for_engine_started(
    process: subprocess.Popen[str],
    timeout: float,
) -> str:
    if process.stdout is None:
        fail("engine stdout pipe was not created")
    output = bytearray()
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            fail(
                "engine did not report readiness before timeout\n"
                + output.decode("utf-8", errors="replace")
            )
        readable, _, _ = select.select(
            [process.stdout], [], [], min(0.1, remaining)
        )
        if readable:
            # Do not call TextIOWrapper.readline() after select(). It may
            # prefetch several lines into Python's private buffer, causing the
            # next select() to wait even though the readiness line is already
            # buffered. Read the OS pipe directly and retain every byte for
            # the final assertions.
            chunk = os.read(process.stdout.fileno(), 65_536)
            if chunk:
                output.extend(chunk)
                if (
                    output.startswith(b"Engine started ")
                    or b"\nEngine started " in output
                ):
                    return output.decode("utf-8", errors="strict")
                continue
        if process.poll() is not None:
            fail(
                f"engine exited before readiness with {process.returncode}\n"
                + output.decode("utf-8", errors="replace")
            )


def assert_header(packet: bytes, sequence: int, message_count: int) -> None:
    if len(packet) < 20:
        fail(f"short MoldUDP64 packet: {len(packet)} bytes")
    if packet[:10] != SESSION:
        fail(f"unexpected session: {packet[:10]!r}")
    actual_sequence, actual_count = struct.unpack(">QH", packet[10:20])
    if (actual_sequence, actual_count) != (sequence, message_count):
        fail(
            "unexpected MoldUDP64 header: "
            f"got sequence/count {(actual_sequence, actual_count)}, "
            f"expected {(sequence, message_count)}"
        )


def assert_system_event(packet: bytes, sequence: int, event_code: bytes) -> None:
    assert_header(packet, sequence, 1)
    if len(packet) != 34:
        fail(f"data packet was {len(packet)} bytes")
    if packet[20:22] != b"\x00\x0c":
        fail(f"unexpected BinaryFILE message length: {packet[20:22]!r}")
    if packet[22:23] != b"S" or packet[33:34] != event_code:
        fail(f"unexpected System Event payload: {packet[22:]!r}")


def recover_redundant_messages(
    packets: list[bytes],
    expected_message_count: int,
    messages_per_packet: int,
    eos_packet_count: int,
) -> tuple[list[bytes], int]:
    if len(packets) < 1 + eos_packet_count:
        fail(f"too few redundant packets: {len(packets)}")

    startup = packets[0]
    if len(startup) != 20:
        fail(f"redundant startup heartbeat was {len(startup)} bytes")
    assert_header(startup, 1, 0)

    next_sequence = 1
    recovered: list[bytes] = []
    data_packets = packets[1:-eos_packet_count]
    for packet in data_packets:
        if len(packet) <= 20:
            fail(f"redundant data packet was only {len(packet)} bytes")
        sequence, message_count = struct.unpack(">QH", packet[10:20])
        if sequence != next_sequence:
            fail(
                f"redundant data sequence {sequence}, "
                f"expected {next_sequence}"
            )
        if message_count == 0 or message_count > messages_per_packet:
            fail(
                f"redundant data message count {message_count}, "
                f"expected 1..{messages_per_packet}"
            )

        offset = 20
        for message_index in range(message_count):
            if offset + 2 > len(packet):
                fail("truncated MoldUDP64 message-length prefix")
            message_length = struct.unpack(">H", packet[offset:offset + 2])[0]
            offset += 2
            message_end = offset + message_length
            if message_end > len(packet):
                fail("truncated MoldUDP64 ITCH payload")
            message = packet[offset:message_end]
            recovered.append(message)
            if (
                len(message) == 12
                and message[0] == ord("S")
                and message[11] in (ord("S"), ord("Q"))
                and message_index + 1 != message_count
            ):
                fail(
                    "System Event S/Q did not end its MoldUDP64 data packet"
                )
            offset = message_end
        if offset != len(packet):
            fail("trailing bytes after MoldUDP64 messages")
        next_sequence += message_count

    if len(recovered) != expected_message_count:
        fail(
            f"recovered {len(recovered)} redundant messages, "
            f"expected {expected_message_count}"
        )
    for packet in packets[-eos_packet_count:]:
        if len(packet) != 20:
            fail(f"redundant EOS packet was {len(packet)} bytes")
        assert_header(packet, next_sequence, 0xFFFF)
    return recovered, len(data_packets)


def aggregate_books(
    symbols: dict[int, str],
    orders: dict[int, tuple[int, str, int, int]],
) -> dict[int, dict[str, list[tuple[int, int, int]]]]:
    raw: dict[int, dict[str, dict[int, list[int]]]] = {
        locate: {"B": {}, "S": {}} for locate in symbols
    }
    for locate, side, quantity, price in orders.values():
        level = raw[locate][side].setdefault(price, [0, 0])
        level[0] += quantity
        level[1] += 1

    books: dict[int, dict[str, list[tuple[int, int, int]]]] = {}
    for locate, sides in raw.items():
        books[locate] = {
            "bids": [
                (price, quantity_and_count[0], quantity_and_count[1])
                for price, quantity_and_count in sorted(
                    sides["B"].items(), reverse=True
                )
            ],
            "asks": [
                (price, quantity_and_count[0], quantity_and_count[1])
                for price, quantity_and_count in sorted(sides["S"].items())
            ],
        }
    return books


def reconstruct_books(
    messages: list[bytes],
) -> tuple[
    dict[int, dict[str, list[tuple[int, int, int]]]],
    dict[int, dict[str, list[tuple[int, int, int]]]],
    dict[int, dict[str, list[tuple[int, int, int]]]],
]:
    symbols: dict[int, str] = {}
    orders: dict[int, tuple[int, str, int, int]] = {}
    after_adds = None
    before_market_end = None
    system_events: list[str] = []

    for message in messages:
        message_type = chr(message[0])
        locate = struct.unpack(">H", message[1:3])[0]
        if message_type == "S":
            system_events.append(chr(message[11]))
            if message[11] == ord("M"):
                before_market_end = aggregate_books(symbols, orders)
            continue
        if message_type == "R":
            ticker = message[11:19].decode("ascii").rstrip(" ")
            if locate in symbols or ticker in symbols.values():
                fail("duplicate identity in redundant integration fixture")
            symbols[locate] = ticker
            continue
        if message_type in ("A", "F"):
            order_ref = struct.unpack(">Q", message[11:19])[0]
            side = chr(message[19])
            quantity = struct.unpack(">I", message[20:24])[0]
            ticker = message[24:32].decode("ascii").rstrip(" ")
            price = struct.unpack(">I", message[32:36])[0]
            if symbols.get(locate) != ticker:
                fail(
                    f"add ticker {ticker!r} does not match locate {locate}"
                )
            if order_ref in orders:
                fail(f"duplicate fixture order reference {order_ref}")
            orders[order_ref] = (locate, side, quantity, price)
            continue

        if after_adds is None:
            after_adds = aggregate_books(symbols, orders)

        order_ref = struct.unpack(">Q", message[11:19])[0]
        if message_type == "U":
            new_order_ref = struct.unpack(">Q", message[19:27])[0]
            quantity = struct.unpack(">I", message[27:31])[0]
            price = struct.unpack(">I", message[31:35])[0]
            if new_order_ref in orders:
                fail(f"duplicate replacement reference {new_order_ref}")
            state = orders.pop(order_ref, None)
            if state is None or state[0] != locate:
                fail(f"unknown replacement reference {order_ref}")
            orders[new_order_ref] = (locate, state[1], quantity, price)
            continue

        state = orders.get(order_ref)
        if state is None or state[0] != locate:
            fail(f"unknown mutation reference {order_ref}")
        if message_type == "D":
            del orders[order_ref]
            continue
        if message_type in ("X", "E", "C"):
            quantity = struct.unpack(">I", message[19:23])[0]
            if quantity <= 0 or quantity > state[2]:
                fail(
                    f"invalid {message_type} quantity {quantity} "
                    f"for order {order_ref}"
                )
            remaining = state[2] - quantity
            if remaining == 0:
                del orders[order_ref]
            else:
                orders[order_ref] = (
                    state[0], state[1], remaining, state[3]
                )
            continue
        fail(f"unhandled book message type {message_type!r}")

    if system_events != ["O", "S", "Q", "M", "E", "C"]:
        fail(f"unexpected lifecycle in redundant fixture: {system_events}")
    if set(symbols.items()) != {(1, "AAPL"), (2, "MSFT")}:
        fail(f"unexpected symbol identities: {symbols}")
    if after_adds is None or before_market_end is None:
        fail("missing aggregate checkpoint in redundant fixture")
    return after_adds, before_market_end, aggregate_books(symbols, orders)


def test_redundant_lifecycle_and_crud(
    binary: Path,
    trace: Path,
    expected_messages: list[bytes],
) -> None:
    messages_per_packet = 4
    eos_packet_count = 2
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "1",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": str(eos_packet_count),
            "ASTRA_LINE_B_DELAY_NS": "0",
        }
    )

    with (
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as line_a,
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as line_b,
    ):
        for listener in (line_a, line_b):
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            listener.bind(("127.0.0.1", 0))
        result = subprocess.run(
            [
                str(binary),
                "--redundant",
                str(trace),
                "127.0.0.1",
                str(line_a.getsockname()[1]),
                str(line_b.getsockname()[1]),
                str(messages_per_packet),
                SESSION.decode("ascii"),
                "0",
                "0",
                "0",
                "off",
                "1.0",
            ],
            env=environment,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        packets_a = receive_all(line_a)
        packets_b = receive_all(line_b)

    if result.returncode != 0:
        fail(
            f"redundant sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if packets_a != packets_b:
        fail(
            "redundant A/B datagrams were not byte-identical: "
            f"line A={len(packets_a)}, line B={len(packets_b)}"
        )

    recovered, data_packet_count = recover_redundant_messages(
        packets_a,
        len(expected_messages),
        messages_per_packet,
        eos_packet_count,
    )
    if recovered != expected_messages:
        fail("redundant sender did not preserve exact BinaryFILE ITCH bytes")

    after_adds, before_market_end, final_books = reconstruct_books(recovered)
    expected_after_adds = {
        1: {
            "bids": [(100_000, 140, 2), (98_000, 25, 1)],
            "asks": [(101_000, 70, 1)],
        },
        2: {
            "bids": [(499_000, 80, 1)],
            "asks": [(500_000, 250, 2), (501_000, 20, 1)],
        },
    }
    expected_before_market_end = {
        1: {
            "bids": [(99_000, 60, 1), (98_000, 25, 1)],
            "asks": [(101_000, 50, 1)],
        },
        2: {
            "bids": [(499_500, 90, 1)],
            "asks": [(500_000, 150, 1), (501_000, 20, 1)],
        },
    }
    expected_final = {
        1: {"bids": [], "asks": []},
        2: {"bids": [], "asks": []},
    }
    if after_adds != expected_after_adds:
        fail(f"wrong aggregate levels after adds: {after_adds}")
    if before_market_end != expected_before_market_end:
        fail(f"wrong aggregate levels before market end: {before_market_end}")
    if final_books != expected_final:
        fail(f"orders survived valid end-of-day cleanup: {final_books}")

    expected_packet_count = 1 + data_packet_count + eos_packet_count
    stats = parse_stats(result.stdout)
    expected_stats = {
        "completion": "complete",
        "line_a_pkts_sent": str(expected_packet_count),
        "line_a_msgs_sent": str(len(expected_messages)),
        "line_a_send_failures": "0",
        "line_b_pkts_sent": str(expected_packet_count),
        "line_b_msgs_sent": str(len(expected_messages)),
        "line_b_send_failures": "0",
        "line_b_delay_ns": "0",
        "logical_packets": str(expected_packet_count),
        "logical_messages": str(len(expected_messages)),
        "startup_heartbeats_sent": "1",
        "periodic_heartbeats_sent": "0",
        "eos_packets_sent": str(eos_packet_count),
        "eos_packets_expected": str(eos_packet_count),
        "first_seq": "1",
        "next_seq": str(len(expected_messages) + 1),
        "binaryfile_completion": "terminator",
        "end_of_session_sent": "true",
    }
    for key, expected in expected_stats.items():
        if stats.get(key) != expected:
            fail(
                f"redundant sender_stats {key}={stats.get(key)!r}, "
                f"expected {expected!r}\n{result.stdout}"
            )


def test_live_engine_redundant_end_to_end(
    sender_binary: Path,
    engine_binary: Path,
    evidence_file: Path,
    trace: Path,
    expected_messages: list[bytes],
) -> None:
    port_a, port_b = reserve_udp_ports(2)
    evidence_sha256 = (
        "a64a97203610a321855988fcc4700e69223b4cd6b4e4bf819861d2f34719bd71"
    )
    engine_environment = blank_environment()
    engine_environment.update(
        {
            "ASTRA_RX": "udp",
            "ASTRA_UDP_RX": "recv",
            "ASTRA_UDP_DROP_METRICS": "off",
            "ASTRA_LATENCY_METRICS": "off",
            "ASTRA_BOOK_CAPACITY_PROFILE":
                "nasdaq-prod-multiday-2026q3-v1",
            "ASTRA_BOOK_CAPACITY_EVIDENCE_FILE": str(evidence_file),
            "ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256": evidence_sha256,
            "ASTRA_ORDER_DIRECT_SLOTS": "1024",
            "ASTRA_ORDER_FALLBACK_BUCKETS": "8",
            "ASTRA_PRICE_PAGE_CAPACITY": "20",
            "ASTRA_PROFILED_MAX_ORDER_REF": "900",
            "ASTRA_PROFILED_UNIQUE_PRICE_PAGES": "10",
            "ASTRA_MIN_DIRECT_ORDER_HEADROOM": "100",
            "ASTRA_MIN_PRICE_PAGE_HEADROOM": "5",
            "ASTRA_BOOK_PREFAULT": "off",
        }
    )
    engine = subprocess.Popen(
        [
            str(engine_binary),
            "127.0.0.1",
            str(port_a),
            "127.0.0.1",
            str(port_b),
        ],
        env=engine_environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    ready_output = ""
    engine_stdout = ""
    engine_stderr = ""
    sender_result: subprocess.CompletedProcess[str] | None = None
    try:
        ready_output = wait_for_engine_started(engine, timeout=30)
        sender_environment = clean_environment()
        sender_environment.update(
            {
                "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
                "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
                "ASTRA_EOS_PACKET_COUNT": "3",
                "ASTRA_EOS_INTERVAL_MS": "0",
                "ASTRA_LINE_B_DELAY_NS": "0",
            }
        )
        sender_result = subprocess.run(
            [
                str(sender_binary),
                "--redundant",
                str(trace),
                "127.0.0.1",
                str(port_a),
                str(port_b),
                "4",
                SESSION.decode("ascii"),
                "0",
                "0",
                "0",
                "off",
                "1.0",
            ],
            env=sender_environment,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        remaining_stdout, engine_stderr = engine.communicate(timeout=10)
        engine_stdout = ready_output + remaining_stdout
    finally:
        if engine.poll() is None:
            engine.terminate()
            try:
                remaining_stdout, engine_stderr = engine.communicate(
                    timeout=2
                )
            except subprocess.TimeoutExpired:
                engine.kill()
                remaining_stdout, engine_stderr = engine.communicate(
                    timeout=2
                )
            engine_stdout = ready_output + remaining_stdout

    if sender_result is None:
        fail("sender was not started after engine readiness")
    if sender_result.returncode != 0:
        fail(
            f"end-to-end sender failed with {sender_result.returncode}\n"
            f"stdout:\n{sender_result.stdout}\n"
            f"stderr:\n{sender_result.stderr}"
        )
    if engine.returncode != 0:
        fail(
            f"end-to-end engine failed with {engine.returncode}\n"
            f"stdout:\n{engine_stdout}\nstderr:\n{engine_stderr}"
        )

    sender_stats = parse_stats(sender_result.stdout)
    if sender_stats.get("completion") != "complete":
        fail(f"end-to-end sender did not complete: {sender_stats}")
    expected_next_sequence = str(len(expected_messages) + 1)
    if sender_stats.get("next_seq") != expected_next_sequence:
        fail(
            f"sender next_seq={sender_stats.get('next_seq')!r}, "
            f"expected {expected_next_sequence}"
        )

    if "Engine stopped  symbols=2" not in engine_stdout:
        fail(f"engine did not report two registered symbols:\n{engine_stdout}")
    engine_stats = parse_record(engine_stdout, "engine_stats")
    expected_engine_stats = {
        "channel_next_seq": expected_next_sequence,
        "channel_status_name": "Good",
        "channel_phase_name": "EndOfMessages",
        "conflicting_buffered_redundant_packets": "0",
        "final_live_orders": "0",
        "registered_symbols": "2",
        "materialized_books": "2",
        "prepared_books": "2",
        "registered_books_missing": "0",
        "unregistered_books_present": "0",
        "descriptor_price_state_mismatches": "0",
        "descriptor_identity_mismatches": "0",
        "committed_price_pages": "2",
        "price_page_capacity": "20",
        "price_page_capacity_failures": "0",
        "end_of_stream_accepted": "true",
    }
    for key, expected in expected_engine_stats.items():
        if engine_stats.get(key) != expected:
            fail(
                f"engine_stats {key}={engine_stats.get(key)!r}, "
                f"expected {expected!r}\n{engine_stdout}"
            )


def test_real_world_control_defaults(binary: Path, trace: Path) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result = subprocess.run(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "1",
                SESSION.decode("ascii"),
                "0",
                "0",
                "0",
                "off",
                "1.0",
            ),
            env=blank_environment(),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        packets = receive_all(listener)

    if result.returncode != 0:
        fail(
            f"default sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(packets) != 12:
        fail(
            "expected two data packets, no startup warmup packets, and "
            f"ten EOS packets; got {len(packets)}\n{result.stdout}"
        )
    assert_system_event(packets[0], 1, b"O")
    assert_system_event(packets[1], 2, b"C")
    for packet in packets[2:]:
        assert_header(packet, 3, 0xFFFF)

    expected_configuration = (
        "startup_heartbeats=0 startup_heartbeat_interval_ms=1000 "
        "heartbeat_interval_ms=1000 eos_packet_count=10 "
        "eos_interval_ms=100 binaryfile_completion=strict"
    )
    if expected_configuration not in result.stdout:
        fail(f"sender did not report real-world defaults:\n{result.stdout}")

    stats = parse_stats(result.stdout)
    expected_stats = {
        "completion": "complete",
        "pkts_sent": "12",
        "msgs_sent": "2",
        "startup_heartbeats_sent": "0",
        "periodic_heartbeats_sent": "0",
        "eos_packets_sent": "10",
        "eos_packets_expected": "10",
        "first_seq": "1",
        "next_seq": "3",
        "send_failures": "0",
        "binaryfile_completion": "terminator",
        "end_of_session_sent": "true",
    }
    for key, expected in expected_stats.items():
        if stats.get(key) != expected:
            fail(
                f"default sender_stats {key}={stats.get(key)!r}, "
                f"expected {expected!r}\n{result.stdout}"
            )


def test_loopback(binary: Path, trace: Path) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        port = str(listener.getsockname()[1])

        result = subprocess.run(
            command(
                binary,
                trace,
                port,
                "1",
                SESSION.decode("ascii"),
                "0",
                "0",
                "0",
                "off",
                "1.0",
            ),
            env=clean_environment(),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        packets = receive_all(listener)

    if result.returncode != 0:
        fail(
            f"sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(packets) != 7:
        fail(
            f"expected 7 loopback datagrams, got {len(packets)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    for packet in packets[:2]:
        if len(packet) != 20:
            fail(f"startup heartbeat was {len(packet)} bytes")
        assert_header(packet, 1, 0)

    assert_system_event(packets[2], 1, b"O")
    assert_system_event(packets[3], 2, b"C")

    for packet in packets[4:]:
        if len(packet) != 20:
            fail(f"end-of-session packet was {len(packet)} bytes")
        assert_header(packet, 3, 0xFFFF)

    required_startup_text = (
        "startup_heartbeats=2 startup_heartbeat_interval_ms=0 "
        "heartbeat_interval_ms=0 eos_packet_count=3 eos_interval_ms=0 "
        "binaryfile_completion=strict"
    )
    if required_startup_text not in result.stdout:
        fail(f"sender did not report parsed environment:\n{result.stdout}")

    expected_stats = {
        "completion": "complete",
        "pkts_sent": "7",
        "msgs_sent": "2",
        "startup_heartbeats_sent": "2",
        "periodic_heartbeats_sent": "0",
        "eos_packets_sent": "3",
        "eos_packets_expected": "3",
        "first_seq": "1",
        "next_seq": "3",
        "send_failures": "0",
        "binaryfile_completion": "terminator",
        "end_of_session_sent": "true",
    }
    stats = parse_stats(result.stdout)
    for key, expected in expected_stats.items():
        if stats.get(key) != expected:
            fail(
                f"sender_stats {key}={stats.get(key)!r}, "
                f"expected {expected!r}\n{result.stdout}"
            )


def test_nonzero_ordinary_packet_rate(binary: Path, trace: Path) -> None:
    packets_per_second = 2
    minimum_observed_gap = 0.35
    maximum_observed_gap = 1.20
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": "1",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result, timed_packets = run_and_receive_timed(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "1",
                SESSION.decode("ascii"),
                str(packets_per_second),
                "0",
                "0",
                "off",
                "1.0",
            ),
            environment,
            listener,
            timeout=5,
        )

    if result.returncode != 0:
        fail(
            f"rate-limited sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(timed_packets) != 7:
        fail(
            "expected six rate-limited data packets and one EOS packet, "
            f"got {len(timed_packets)}\n{result.stdout}"
        )

    event_codes = (b"O", b"S", b"Q", b"M", b"E", b"C")
    for index, event_code in enumerate(event_codes):
        packet, _ = timed_packets[index]
        assert_system_event(packet, index + 1, event_code)
    assert_header(timed_packets[-1][0], 7, 0xFFFF)

    for index in range(1, len(event_codes)):
        observed_gap = (
            timed_packets[index][1] - timed_packets[index - 1][1]
        )
        if not minimum_observed_gap <= observed_gap <= maximum_observed_gap:
            fail(
                f"ordinary packet-rate gap {index} was "
                f"{observed_gap:.3f}s; expected "
                f"{minimum_observed_gap:.2f}.."
                f"{maximum_observed_gap:.2f}s"
            )

    stats = parse_stats(result.stdout)
    if stats.get("periodic_heartbeats_sent") != "0":
        fail(f"unexpected heartbeat during ordinary rate test: {stats}")
    if stats.get("completion") != "complete":
        fail(f"ordinary rate replay did not complete: {stats}")


def test_completed_session_sends_eos_without_rate_wait(
    binary: Path,
    trace: Path,
) -> None:
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "100",
            "ASTRA_EOS_PACKET_COUNT": "1",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result, timed_packets = run_and_receive_timed(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "20",
                SESSION.decode("ascii"),
                "1",
                "0",
                "0",
                "off",
                "1.0",
            ),
            environment,
            listener,
            timeout=3,
        )

    if result.returncode != 0:
        fail(
            f"completed-session sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(timed_packets) != 2:
        headers = [
            struct.unpack(">QH", packet[10:20])
            for packet, _ in timed_packets
            if len(packet) >= 20
        ]
        fail(
            "a completed source must transition directly from its final "
            f"data packet to EOS; got headers {headers}\n{result.stdout}"
        )

    final_data, final_data_at = timed_packets[0]
    end_of_session, end_of_session_at = timed_packets[1]
    assert_header(final_data, 1, 2)
    assert_header(end_of_session, 3, 0xFFFF)
    eos_delay = end_of_session_at - final_data_at
    if eos_delay >= 0.50:
        fail(
            f"EOS followed completed data after {eos_delay:.3f}s; "
            "the configured one-second data interval must not delay EOS"
        )

    stats = parse_stats(result.stdout)
    if stats.get("periodic_heartbeats_sent") != "0":
        fail(f"heartbeat was sent after known completion: {stats}")


def test_ss_pause_off_mode_does_not_add_rate_interval(
    binary: Path,
    trace: Path,
) -> None:
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": "1",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result, timed_packets = run_and_receive_timed(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "20",
                SESSION.decode("ascii"),
                "1",
                "0",
                "1",
                "off",
                "1.0",
            ),
            environment,
            listener,
            timeout=5,
        )

    if result.returncode != 0:
        fail(
            f"SS-pause/rate sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(timed_packets) != 3:
        fail(
            "expected SO/SS data, SC data, and EOS for SS-pause/rate "
            f"test; got {len(timed_packets)}\n{result.stdout}"
        )

    assert_header(timed_packets[0][0], 1, 2)
    assert_system_event(timed_packets[1][0], 3, b"C")
    assert_header(timed_packets[2][0], 4, 0xFFFF)
    observed_pause = timed_packets[1][1] - timed_packets[0][1]
    if observed_pause < 0.75:
        fail(f"configured one-second SS pause lasted {observed_pause:.3f}s")
    if observed_pause >= 1.65:
        fail(
            f"SS pause lasted {observed_pause:.3f}s; off mode appears to "
            "have added the one-second ordinary packet interval"
        )


def test_timestamp_premarket_and_sq_rate_handoff(
    binary: Path,
    trace: Path,
) -> None:
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": "1",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result, timed_packets = run_and_receive_timed(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "20",
                SESSION.decode("ascii"),
                "2",
                "0",
                "0",
                "timestamp",
                "2.0",
            ),
            environment,
            listener,
            timeout=5,
        )

    if result.returncode != 0:
        fail(
            f"timestamp replay failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(timed_packets) != 6:
        fail(
            "expected five timestamp-mode data packets and EOS, "
            f"got {len(timed_packets)}\n{result.stdout}"
        )

    packets = [packet for packet, _ in timed_packets]
    assert_header(packets[0], 1, 2)
    if (
        len(packets[0]) != 48
        or packets[0][22:23] != b"S"
        or packets[0][33:34] != b"O"
        or packets[0][36:37] != b"S"
        or packets[0][47:48] != b"S"
    ):
        fail(f"unexpected combined SO/SS packet: {packets[0]!r}")
    assert_header(packets[1], 3, 1)
    assert_header(packets[2], 4, 1)
    assert_system_event(packets[3], 5, b"Q")
    assert_system_event(packets[4], 6, b"C")
    assert_header(packets[5], 7, 0xFFFF)

    # Although the configured ceiling is 20, each timestamp-paced message
    # between SS and SQ must be its own packet. Otherwise the second directory
    # would be released at the first directory's earlier timestamp.
    if packets[1][22:23] != b"R" or packets[2][22:23] != b"R":
        fail("timestamp-mode one-message packetization was not preserved")

    ss_received_at = timed_packets[0][1]
    expected_scaled_offsets = (0.10, 0.20, 0.30)
    for packet_index, expected_offset in zip(
        (1, 2, 3), expected_scaled_offsets
    ):
        observed_offset = timed_packets[packet_index][1] - ss_received_at
        if observed_offset < expected_offset - 0.07:
            fail(
                f"timestamp packet {packet_index + 1} arrived at "
                f"{observed_offset:.3f}s after SS, earlier than its "
                f"{expected_offset:.2f}s scaled deadline"
            )
        if observed_offset > expected_offset + 0.50:
            fail(
                f"timestamp packet {packet_index + 1} arrived at "
                f"{observed_offset:.3f}s after SS, too late for its "
                f"{expected_offset:.2f}s scaled deadline"
            )

    post_sq_gap = timed_packets[4][1] - timed_packets[3][1]
    if not 0.35 <= post_sq_gap <= 1.20:
        fail(
            f"post-SQ ordinary-rate gap was {post_sq_gap:.3f}s; "
            "expected approximately 500ms"
        )


def test_idle_heartbeat_during_ss_pause(
    binary: Path,
    trace: Path,
) -> None:
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "200",
            "ASTRA_EOS_PACKET_COUNT": "1",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        port = str(listener.getsockname()[1])
        started_at = time.monotonic()
        result = subprocess.run(
            command(
                binary,
                trace,
                port,
                "20",
                SESSION.decode("ascii"),
                "0",
                "0",
                "1",
                "off",
                "1.0",
            ),
            env=environment,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        elapsed = time.monotonic() - started_at
        packets = receive_all(listener)

    if result.returncode != 0:
        fail(
            f"SS-pause sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if elapsed < 0.8:
        fail(f"configured one-second SS pause lasted only {elapsed:.3f}s")
    if len(packets) < 5:
        fail(
            "expected two data packets, at least two idle heartbeats, "
            f"and EOS; got {len(packets)} packets\n{result.stdout}"
        )

    assert_header(packets[0], 1, 2)
    if len(packets[0]) != 48:
        fail(f"combined SO/SS packet was {len(packets[0])} bytes")
    if (
        packets[0][20:22] != b"\x00\x0c"
        or packets[0][22:23] != b"S"
        or packets[0][33:34] != b"O"
        or packets[0][34:36] != b"\x00\x0c"
        or packets[0][36:37] != b"S"
        or packets[0][47:48] != b"S"
    ):
        fail(f"unexpected SO/SS packet payload: {packets[0][20:]!r}")
    assert_system_event(packets[-2], 3, b"C")
    assert_header(packets[-1], 4, 0xFFFF)

    heartbeat_packets = packets[1:-2]
    for packet in heartbeat_packets:
        if len(packet) != 20:
            fail(f"idle heartbeat was {len(packet)} bytes")
        # Sequence 3 is the SC record after SS. With a 20-message packet
        # ceiling, these heartbeats prove SS ended its packet and the sender
        # entered the pause before reading/sending the following record.
        assert_header(packet, 3, 0)

    stats = parse_stats(result.stdout)
    periodic_count = len(heartbeat_packets)
    expected_stats = {
        "completion": "complete",
        "pkts_sent": str(len(packets)),
        "msgs_sent": "3",
        "startup_heartbeats_sent": "0",
        "periodic_heartbeats_sent": str(periodic_count),
        "eos_packets_sent": "1",
        "eos_packets_expected": "1",
        "first_seq": "1",
        "next_seq": "4",
        "send_failures": "0",
        "binaryfile_completion": "terminator",
        "end_of_session_sent": "true",
    }
    for key, expected in expected_stats.items():
        if stats.get(key) != expected:
            fail(
                f"SS-pause sender_stats {key}={stats.get(key)!r}, "
                f"expected {expected!r}\n{result.stdout}"
            )


def test_strict_missing_terminator(
    binary: Path,
    trace: Path,
) -> None:
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": "2",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result = subprocess.run(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "1",
                SESSION.decode("ascii"),
                "0",
                "0",
                "0",
                "off",
                "1.0",
            ),
            env=environment,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        packets = receive_all(listener)

    if result.returncode == 0:
        fail(
            "strict trace without terminator unexpectedly succeeded\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if "physical EOF before zero-length BinaryFILE terminator" not in (
        result.stderr
    ):
        fail(f"missing strict EOF diagnostic:\n{result.stderr}")
    if len(packets) != 2:
        fail(
            f"expected only the two data packets, got {len(packets)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    assert_system_event(packets[0], 1, b"O")
    assert_system_event(packets[1], 2, b"C")
    if any(struct.unpack(">H", packet[18:20])[0] == 0xFFFF
           for packet in packets):
        fail("sender emitted EOS for an incomplete strict BinaryFILE")

    stats = parse_stats(result.stdout)
    expected_stats = {
        "completion": "source_error",
        "pkts_sent": "2",
        "msgs_sent": "2",
        "startup_heartbeats_sent": "0",
        "periodic_heartbeats_sent": "0",
        "eos_packets_sent": "0",
        "eos_packets_expected": "2",
        "first_seq": "1",
        "next_seq": "3",
        "send_failures": "0",
        "binaryfile_completion": "none",
        "end_of_session_sent": "false",
    }
    for key, expected in expected_stats.items():
        if stats.get(key) != expected:
            fail(
                f"incomplete sender_stats {key}={stats.get(key)!r}, "
                f"expected {expected!r}\n{result.stdout}"
            )


def test_legacy_sc_eof_completion(
    binary: Path,
    trace: Path,
) -> None:
    environment = clean_environment()
    environment.update(
        {
            "ASTRA_STARTUP_HEARTBEAT_COUNT": "0",
            "ASTRA_HEARTBEAT_INTERVAL_MS": "0",
            "ASTRA_EOS_PACKET_COUNT": "1",
            "ASTRA_BINARYFILE_COMPLETION": "legacy-sc-eof",
        }
    )

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        listener.bind(("127.0.0.1", 0))
        result = subprocess.run(
            command(
                binary,
                trace,
                str(listener.getsockname()[1]),
                "1",
                SESSION.decode("ascii"),
                "0",
                "0",
                "0",
                "off",
                "1.0",
            ),
            env=environment,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        packets = receive_all(listener)

    if result.returncode != 0:
        fail(
            f"legacy SC+EOF sender failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if len(packets) != 3:
        fail(
            "expected two data packets and one EOS packet for legacy "
            f"completion; got {len(packets)}\n{result.stdout}"
        )
    assert_system_event(packets[0], 1, b"O")
    assert_system_event(packets[1], 2, b"C")
    assert_header(packets[2], 3, 0xFFFF)

    stats = parse_stats(result.stdout)
    expected_stats = {
        "completion": "complete",
        "pkts_sent": "3",
        "msgs_sent": "2",
        "startup_heartbeats_sent": "0",
        "periodic_heartbeats_sent": "0",
        "eos_packets_sent": "1",
        "eos_packets_expected": "1",
        "first_seq": "1",
        "next_seq": "3",
        "send_failures": "0",
        "binaryfile_completion": "legacy_sc_eof",
        "end_of_session_sent": "true",
    }
    for key, expected in expected_stats.items():
        if stats.get(key) != expected:
            fail(
                f"legacy sender_stats {key}={stats.get(key)!r}, "
                f"expected {expected!r}\n{result.stdout}"
            )


def expect_rejected(
    binary: Path,
    trace: Path,
    arguments: list[str],
    expected_error: str,
    environment_override: dict[str, str] | None = None,
) -> None:
    environment = clean_environment()
    if environment_override:
        environment.update(environment_override)
    result = subprocess.run(
        [str(binary), str(trace), "127.0.0.1", *arguments],
        env=environment,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    if result.returncode == 0:
        fail(
            f"invalid invocation unexpectedly succeeded: {arguments!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if expected_error not in result.stderr:
        fail(
            f"missing error {expected_error!r} for {arguments!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def expect_raw_rejected(
    arguments: list[str],
    expected_error: str,
    environment: dict[str, str],
) -> None:
    result = subprocess.run(
        arguments,
        env=environment,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    if result.returncode == 0:
        fail(
            f"invalid invocation unexpectedly succeeded: {arguments!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if expected_error not in result.stderr:
        fail(
            f"missing error {expected_error!r} for {arguments!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def test_invalid_configuration(binary: Path, trace: Path) -> None:
    expect_rejected(binary, trace, ["65537"], "port must be an integer")
    expect_rejected(binary, trace, ["123junk"], "port must be an integer")
    expect_rejected(
        binary,
        trace,
        ["12345", "65537"],
        "msgs_per_packet must be an integer",
    )
    expect_rejected(
        binary,
        trace,
        ["12345", "2junk"],
        "msgs_per_packet must be an integer",
    )
    expect_rejected(
        binary,
        trace,
        ["12345", "1", "SESSION-IS-TOO-LONG"],
        "session must not exceed 10 bytes",
    )
    expect_rejected(
        binary,
        trace,
        ["12345", "1", "TESTSESS01", "1000000001"],
        "pkt/s must be an integer",
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "ASTRA_STARTUP_HEARTBEAT_COUNT must be an integer",
        {"ASTRA_STARTUP_HEARTBEAT_COUNT": "1000001"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS must be an integer",
        {"ASTRA_STARTUP_HEARTBEAT_INTERVAL_MS": "60001"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "ASTRA_HEARTBEAT_INTERVAL_MS must be an integer",
        {"ASTRA_HEARTBEAT_INTERVAL_MS": "60001"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "ASTRA_EOS_PACKET_COUNT must be an integer",
        {"ASTRA_EOS_PACKET_COUNT": "0"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "ASTRA_EOS_INTERVAL_MS must be an integer",
        {"ASTRA_EOS_INTERVAL_MS": "60001"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "premarket_seconds must be an integer",
        {"ASTRA_PREMARKET_SECONDS": "not-a-number"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "premarket_replay_mode must be",
        {"ASTRA_PREMARKET_REPLAY_MODE": "warp"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "premarket_speedup must be a finite number",
        {"ASTRA_PREMARKET_SPEEDUP": "nan"},
    )
    expect_rejected(
        binary,
        trace,
        ["12345"],
        "ASTRA_BINARYFILE_COMPLETION must be",
        {"ASTRA_BINARYFILE_COMPLETION": "auto"},
    )
    expect_raw_rejected(
        [
            str(binary),
            "--redundant",
            str(trace),
            "127.0.0.1",
            "12345",
            "65537",
            "1",
            "TESTSESS01",
            "0",
        ],
        "port_b must be an integer",
        clean_environment(),
    )
    expect_raw_rejected(
        [
            str(binary),
            "--redundant",
            str(trace),
            "127.0.0.1",
            "12345",
            "12345",
            "1",
            "TESTSESS01",
            "0",
        ],
        "port_a and port_b must be distinct",
        clean_environment(),
    )


def main() -> int:
    if len(sys.argv) != 4:
        print(
            f"usage: {Path(sys.argv[0]).name} <itch_moldudp_sender> "
            "<md_engine> <capacity_evidence>",
            file=sys.stderr,
        )
        return 2

    binary = Path(sys.argv[1]).resolve()
    engine_binary = Path(sys.argv[2]).resolve()
    evidence_file = Path(sys.argv[3]).resolve()
    if not binary.is_file():
        print(f"sender binary not found: {binary}", file=sys.stderr)
        return 2
    if not engine_binary.is_file():
        print(f"engine binary not found: {engine_binary}", file=sys.stderr)
        return 2
    if not evidence_file.is_file():
        print(f"capacity evidence not found: {evidence_file}",
              file=sys.stderr)
        return 2

    try:
        with tempfile.TemporaryDirectory(
            prefix="astra-sender-integration-"
        ) as directory:
            trace = Path(directory) / "strict.binaryfile"
            write_trace(trace)
            test_real_world_control_defaults(binary, trace)
            test_loopback(binary, trace)
            test_completed_session_sends_eos_without_rate_wait(binary, trace)
            test_invalid_configuration(binary, trace)

            rate_trace = Path(directory) / "ordinary-rate.binaryfile"
            write_trace(rate_trace, ("O", "S", "Q", "M", "E", "C"))
            test_nonzero_ordinary_packet_rate(binary, rate_trace)

            lifecycle_messages = lifecycle_crud_messages()
            lifecycle_trace = Path(directory) / "lifecycle-crud.binaryfile"
            write_messages(lifecycle_trace, lifecycle_messages)
            test_redundant_lifecycle_and_crud(
                binary, lifecycle_trace, lifecycle_messages
            )
            test_live_engine_redundant_end_to_end(
                binary,
                engine_binary,
                evidence_file,
                lifecycle_trace,
                lifecycle_messages,
            )

            pause_trace = Path(directory) / "ss-pause.binaryfile"
            write_trace(pause_trace, ("O", "S", "C"))
            test_idle_heartbeat_during_ss_pause(binary, pause_trace)
            test_ss_pause_off_mode_does_not_add_rate_interval(
                binary, pause_trace
            )

            timestamp_trace = Path(directory) / "timestamp-premarket.binaryfile"
            write_messages(
                timestamp_trace,
                [
                    system_event("O", 100_000_000),
                    system_event("S", 1_000_000_000),
                    with_timestamp(
                        stock_directory(1, "AAPL"), 1_200_000_000
                    ),
                    with_timestamp(
                        stock_directory(2, "MSFT"), 1_400_000_000
                    ),
                    system_event("Q", 1_600_000_000),
                    system_event("C", 1_700_000_000),
                ],
            )
            test_timestamp_premarket_and_sq_rate_handoff(
                binary, timestamp_trace
            )

            incomplete_trace = Path(directory) / "missing-terminator.binaryfile"
            write_trace(incomplete_trace, terminator=False)
            test_strict_missing_terminator(binary, incomplete_trace)
            test_legacy_sc_eof_completion(binary, incomplete_trace)
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print(
        "sender defaults, single and redundant loopback framing, live-engine "
        "end-to-end lifecycle/CRUD/book audit, ordinary/timestamp pacing, SS "
        "transitions, idle heartbeats, strict and legacy completion, counters, "
        "and validation: PASS"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
