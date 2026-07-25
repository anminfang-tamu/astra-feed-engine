#!/usr/bin/env python3

import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path


CLEAN_PROFILE = """\
itch_trace_profile path="{binaryfile_path}" records=100 bytes=12345 malformed_records=0 zero_length_records={zero_length_records} binaryfile_completion={completion} max_message_length=48 max_stock_locate=7 max_order_ref=900 order_refs_above_u32=0 max_match_number=12 min_price=100 max_price=200 prices_above_documented_max=0 distinct_locate_price_pages=10 binaryfile_sha256={binaryfile_sha256} profiler_sha256={profiler_sha256}
lifecycle directory_before_system_hours=1 directory_after_system_hours=0 new_directory_after_system_hours=0 repeated_directory_after_system_hours=0 conflicting_directory_after_system_hours=0 conflicting_directory_identities=0 invalid_directory_identities=0 duplicate_directory_tickers=0 invalid_timestamps=0 order_adds_before_directory=0 order_add_stock_mismatches=0 book_messages_before_system_hours=0 system_event_e=1 daily_system_events_seen=6 system_event_sequence_errors=0 unsupported_system_event_codes=0 messages_after_system_event_e=1 cancels_after_system_event_e=0 deletes_after_system_event_e=1 broken_trades_after_system_event_e=0 invalid_messages_after_system_event_e=0
orders new_refs=8 id_min=100 id_max=900 id_gt_u32=0 monotonic_fraction=1 max_forward_gap=100 live_final=0 live_hwm=4 duplicates=0 missing_D=0 missing_X=0 missing_EC=0 missing_U=0 same_ref_U=0 over_X=0 over_EC=0 zero_qty=0 invalid_sides=0 locate_mismatches=0 invariant_failures=0
prices adds=8 symbols=1 min=100 max=200 outside_first_65536_window=0 active_levels_final=0 active_levels_hwm=3 active_price_pages_final=0 active_price_pages_hwm=2 ever_price_pages=10 max_level_qty=1000
certification semantic_clean=1
"""

CORPUS_MANIFEST = """\
schema=astra_itch_trace_manifest_v1
source_url=https://example.invalid/fixture.itch.gz
trade_date=2026-06-12
binaryfile_path={binaryfile_path}
binaryfile_bytes=12345
binaryfile_sha256={binaryfile_sha256}
first_itch_message=system_event_O
final_itch_message=system_event_C
zero_length_terminator_present=false
"""


def invoke(
    generator: Path,
    profile: Path,
    corpus: Path,
    binaryfile: Path,
    profiler: Path,
    output: Path,
    *,
    fallback: str = "8",
    direct_headroom: str = "123",
    page_headroom: str = "7",
    profile_name: str = "nasdaq-test-20260612-v1",
    expect_success: bool,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [
            sys.executable,
            str(generator),
            "--profile-output",
            str(profile),
            "--corpus-manifest",
            str(corpus),
            "--binaryfile",
            str(binaryfile),
            "--profiler-binary",
            str(profiler),
            "--profile-name",
            profile_name,
            "--minimum-direct-order-headroom",
            direct_headroom,
            "--minimum-price-page-headroom",
            page_headroom,
            "--order-fallback-buckets",
            fallback,
            "--output",
            str(output),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if (result.returncode == 0) != expect_success:
        raise AssertionError(
            f"unexpected generator exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def write_inputs(directory: Path) -> tuple[Path, Path, Path, Path]:
    profile = directory / "trace.profile.txt"
    corpus = directory / "trace.manifest.txt"
    binaryfile = directory / "fixture.itch"
    profiler = directory / "astra_itch_trace_profile"
    binaryfile.write_bytes(b"x" * 12345)
    profiler.write_text(
        """#!/usr/bin/env python3
import hashlib
import sys
from pathlib import Path

target = Path(sys.argv[1])
legacy = len(sys.argv) == 3 and sys.argv[2] == "--allow-legacy-eof-after-sc"
if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and not legacy):
    raise SystemExit(2)
binaryfile_sha256 = hashlib.sha256(target.read_bytes()).hexdigest()
profiler_sha256 = hashlib.sha256(Path(sys.argv[0]).read_bytes()).hexdigest()
completion = "legacy_sc_eof" if legacy else "terminator"
zero_length_records = 0 if legacy else 1
print(
    f'itch_trace_profile path="{sys.argv[1]}" records=100 bytes=12345 '
    f'malformed_records=0 zero_length_records={zero_length_records} '
    f'binaryfile_completion={completion} max_message_length=48 '
    f'max_stock_locate=7 max_order_ref=900 order_refs_above_u32=0 '
    f'max_match_number=12 min_price=100 max_price=200 '
    f'prices_above_documented_max=0 distinct_locate_price_pages=10 '
    f'binaryfile_sha256={binaryfile_sha256} profiler_sha256={profiler_sha256}'
)
print(
    "lifecycle directory_before_system_hours=1 "
    "directory_after_system_hours=0 new_directory_after_system_hours=0 "
    "repeated_directory_after_system_hours=0 "
    "conflicting_directory_after_system_hours=0 "
    "conflicting_directory_identities=0 invalid_directory_identities=0 "
    "duplicate_directory_tickers=0 invalid_timestamps=0 "
    "order_adds_before_directory=0 order_add_stock_mismatches=0 "
    "book_messages_before_system_hours=0 system_event_e=1 "
    "daily_system_events_seen=6 system_event_sequence_errors=0 "
            "unsupported_system_event_codes=0 messages_after_system_event_e=1 "
            "cancels_after_system_event_e=0 "
    "deletes_after_system_event_e=1 broken_trades_after_system_event_e=0 "
    "invalid_messages_after_system_event_e=0"
)
print(
    "orders new_refs=8 id_min=100 id_max=900 id_gt_u32=0 "
    "monotonic_fraction=1 max_forward_gap=100 live_final=0 live_hwm=4 "
    "duplicates=0 missing_D=0 missing_X=0 missing_EC=0 missing_U=0 "
    "same_ref_U=0 over_X=0 over_EC=0 zero_qty=0 invalid_sides=0 "
    "locate_mismatches=0 invariant_failures=0"
)
print(
    "prices adds=8 symbols=1 min=100 max=200 "
    "outside_first_65536_window=0 active_levels_final=0 "
    "active_levels_hwm=3 active_price_pages_final=0 "
    "active_price_pages_hwm=2 ever_price_pages=10 max_level_qty=1000"
)
print("certification semantic_clean=1")
""",
        encoding="ascii",
    )
    profiler.chmod(0o755)
    binaryfile_sha256 = hashlib.sha256(binaryfile.read_bytes()).hexdigest()
    profiler_sha256 = hashlib.sha256(profiler.read_bytes()).hexdigest()
    profile.write_text(
        CLEAN_PROFILE.format(
            binaryfile_path=str(binaryfile),
            binaryfile_sha256=binaryfile_sha256,
            profiler_sha256=profiler_sha256,
            zero_length_records=0,
            completion="legacy_sc_eof",
        ),
        encoding="ascii",
    )
    corpus.write_text(
        CORPUS_MANIFEST.format(
            binaryfile_path=str(binaryfile),
            binaryfile_sha256=binaryfile_sha256
        ),
        encoding="ascii",
    )
    return profile, corpus, binaryfile, profiler


def expect_failure(
    generator: Path,
    profile: Path,
    corpus: Path,
    binaryfile: Path,
    profiler: Path,
    output: Path,
    expected: str,
    **overrides: str,
) -> None:
    result = invoke(
        generator,
        profile,
        corpus,
        binaryfile,
        profiler,
        output,
        expect_success=False,
        **overrides,
    )
    if expected not in result.stderr:
        raise AssertionError(
            f"expected failure containing {expected!r}\n"
            f"stderr:\n{result.stderr}"
        )


def check_engine_accepts(
    engine: Path, evidence: Path, evidence_sha256: str
) -> None:
    profile_output_sha256 = next(
        line.split("=", 1)[1]
        for line in evidence.read_text(encoding="ascii").splitlines()
        if line.startswith("profile_output_sha256=")
    )
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.startswith("ASTRA_") and (
            "CAPACITY" in key or key == "ASTRA_BOOK_PREFAULT"
        ):
            environment.pop(key)
    environment.update(
        {
            "ASTRA_BOOK_CAPACITY_PROFILE": "nasdaq-test-20260612-v1",
            "ASTRA_BOOK_CAPACITY_EVIDENCE_FILE": str(evidence),
            "ASTRA_BOOK_CAPACITY_EVIDENCE_SHA256": evidence_sha256,
            "ASTRA_ORDER_DIRECT_SLOTS": "1024",
            "ASTRA_ORDER_FALLBACK_BUCKETS": "8",
            "ASTRA_PRICE_PAGE_CAPACITY": "17",
            "ASTRA_PROFILED_MAX_ORDER_REF": "900",
            "ASTRA_PROFILED_UNIQUE_PRICE_PAGES": "10",
            "ASTRA_MIN_DIRECT_ORDER_HEADROOM": "123",
            "ASTRA_MIN_PRICE_PAGE_HEADROOM": "7",
            "ASTRA_BOOK_PREFAULT": "off",
        }
    )
    result = subprocess.run(
        [str(engine), "--book-storage-plan-only"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"engine rejected generated evidence\nstdout:\n{result.stdout}"
            f"\nstderr:\n{result.stderr}"
        )
    assert "order_direct_slots=1024" in result.stdout
    assert "evidence_schema=astra_book_capacity_evidence_v2" in result.stdout
    assert (
        f"profile_output_sha256={profile_output_sha256}" in result.stdout
    )
    assert "order_fallback_buckets=8" in result.stdout
    assert "price_page_capacity=17" in result.stdout
    assert "effective_direct_order_headroom=123" in result.stdout
    assert "effective_price_page_headroom=7" in result.stdout


def main() -> int:
    if len(sys.argv) not in (2, 3):
        raise SystemExit(
            "usage: DeriveBookCapacityEvidenceTest.py "
            "<derive-script> [md-engine]"
        )
    generator = Path(sys.argv[1])
    engine = Path(sys.argv[2]) if len(sys.argv) == 3 else None

    with tempfile.TemporaryDirectory(
        prefix="astra-capacity-evidence-"
    ) as temp:
        directory = Path(temp)
        profile, corpus, binaryfile, profiler = write_inputs(directory)
        output = directory / "capacity-evidence.txt"

        created = invoke(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            output,
            expect_success=True,
        )
        assert "status=created" in created.stdout
        clean_profile = profile.read_text(encoding="ascii")
        corpus_sha256 = hashlib.sha256(corpus.read_bytes()).hexdigest()
        profiler_sha256 = hashlib.sha256(profiler.read_bytes()).hexdigest()
        profile_output_sha256 = hashlib.sha256(
            profile.read_bytes()
        ).hexdigest()
        expected = (
            "schema=astra_book_capacity_evidence_v2\n"
            "profile_name=nasdaq-test-20260612-v1\n"
            f"corpus_manifest_sha256={corpus_sha256}\n"
            f"profiler_sha256={profiler_sha256}\n"
            f"profile_output_sha256={profile_output_sha256}\n"
            "order_direct_slots=1024\n"
            "order_fallback_buckets=8\n"
            "price_page_capacity=17\n"
            "profiled_max_order_ref=900\n"
            "profiled_unique_price_pages=10\n"
            "minimum_direct_order_headroom=123\n"
            "minimum_price_page_headroom=7\n"
        ).encode("ascii")
        assert output.read_bytes() == expected
        evidence_sha256 = hashlib.sha256(expected).hexdigest()
        binaryfile_sha256 = hashlib.sha256(
            binaryfile.read_bytes()
        ).hexdigest()
        assert f"sha256={evidence_sha256}" in created.stdout
        assert f"corpus_manifest_sha256={corpus_sha256}" in created.stdout
        assert f"binaryfile_sha256={binaryfile_sha256}" in created.stdout
        assert f"profiler_sha256={profiler_sha256}" in created.stdout
        assert (
            f"profile_output_sha256={profile_output_sha256}"
            in created.stdout
        )

        unchanged = invoke(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            output,
            expect_success=True,
        )
        assert "status=unchanged" in unchanged.stdout
        assert output.read_bytes() == expected
        if engine is not None:
            check_engine_accepts(engine, output, evidence_sha256)

        forged_capacity = directory / "forged-capacity.profile.txt"
        forged_capacity.write_text(
            clean_profile.replace(
                "max_order_ref=900", "max_order_ref=800", 1
            ).replace("id_max=900", "id_max=800", 1),
            encoding="ascii",
        )
        expect_failure(
            generator,
            forged_capacity,
            corpus,
            binaryfile,
            profiler,
            directory / "forged-capacity.txt",
            "does not exactly match frozen profiler re-execution",
        )

        profiler.chmod(0o644)
        expect_failure(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            directory / "non-executable-profiler.txt",
            "profiler binary is not executable",
        )
        profiler.chmod(0o755)

        terminated_profile = directory / "terminated.profile.txt"
        terminated_profile.write_text(
            clean_profile.replace(
                "zero_length_records=0 "
                "binaryfile_completion=legacy_sc_eof",
                "zero_length_records=1 binaryfile_completion=terminator",
            ),
            encoding="ascii",
        )
        terminated_corpus = directory / "terminated.manifest.txt"
        terminated_corpus.write_text(
            corpus.read_text(encoding="ascii").replace(
                "zero_length_terminator_present=false",
                "zero_length_terminator_present=true",
            ),
            encoding="ascii",
        )
        terminated_output = directory / "terminated-evidence.txt"
        terminated = invoke(
            generator,
            terminated_profile,
            terminated_corpus,
            binaryfile,
            profiler,
            terminated_output,
            profile_name="nasdaq-test-terminated-v1",
            expect_success=True,
        )
        assert "status=created" in terminated.stdout
        assert (
            "profile_name=nasdaq-test-terminated-v1"
            in terminated_output.read_text(encoding="ascii")
        )

        dirty_profile = directory / "dirty.profile.txt"
        dirty_profile.write_text(
            clean_profile.replace(
                "certification semantic_clean=1",
                "certification semantic_clean=0",
            ),
            encoding="ascii",
        )
        expect_failure(
            generator,
            dirty_profile,
            corpus,
            binaryfile,
            profiler,
            directory / "dirty.txt",
            "semantic_clean is not 1",
        )

        hidden_anomaly = directory / "hidden-anomaly.profile.txt"
        hidden_anomaly.write_text(
            clean_profile.replace("duplicates=0", "duplicates=1", 1),
            encoding="ascii",
        )
        expect_failure(
            generator,
            hidden_anomaly,
            corpus,
            binaryfile,
            profiler,
            directory / "hidden-anomaly.txt",
            "orders.duplicates is nonzero",
        )

        invalid_timestamp = directory / "invalid-timestamp.profile.txt"
        invalid_timestamp.write_text(
            clean_profile.replace(
                "invalid_timestamps=0", "invalid_timestamps=1", 1
            ),
            encoding="ascii",
        )
        expect_failure(
            generator,
            invalid_timestamp,
            corpus,
            binaryfile,
            profiler,
            directory / "invalid-timestamp.txt",
            "lifecycle.invalid_timestamps is nonzero",
        )

        incomplete = directory / "incomplete.profile.txt"
        incomplete.write_text(
            clean_profile.replace(
                "binaryfile_completion=legacy_sc_eof",
                "binaryfile_completion=incomplete_eof",
            ),
            encoding="ascii",
        )
        expect_failure(
            generator,
            incomplete,
            corpus,
            binaryfile,
            profiler,
            directory / "incomplete.txt",
            "completion is not terminator or legacy_sc_eof",
        )

        inconsistent_ref = directory / "inconsistent-ref.profile.txt"
        inconsistent_ref.write_text(
            clean_profile.replace("id_max=900", "id_max=899"),
            encoding="ascii",
        )
        expect_failure(
            generator,
            inconsistent_ref,
            corpus,
            binaryfile,
            profiler,
            directory / "inconsistent-ref.txt",
            "max_order_ref and orders.id_max are inconsistent",
        )

        wrong_corpus = directory / "wrong-corpus.manifest.txt"
        wrong_corpus.write_text(
            corpus.read_text(encoding="ascii").replace(
                "binaryfile_bytes=12345", "binaryfile_bytes=12346"
            ),
            encoding="ascii",
        )
        expect_failure(
            generator,
            profile,
            wrong_corpus,
            binaryfile,
            profiler,
            directory / "wrong-corpus.txt",
            "byte count differs",
        )

        expect_failure(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            directory / "fallback.txt",
            "power of two",
            fallback="3",
        )
        expect_failure(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            directory / "zero-headroom.txt",
            "argument must be positive",
            direct_headroom="0",
        )
        expect_failure(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            directory / "bad-name.txt",
            "audit-safe token",
            profile_name="bad profile",
        )

        occupied = directory / "occupied.txt"
        occupied.write_bytes(b"different\n")
        expect_failure(
            generator,
            profile,
            corpus,
            binaryfile,
            profiler,
            occupied,
            "refusing to replace different existing output",
        )

        tampered_binaryfile = directory / "tampered-same-size.itch"
        tampered_binaryfile.write_bytes(b"y" + binaryfile.read_bytes()[1:])
        expect_failure(
            generator,
            profile,
            corpus,
            tampered_binaryfile,
            profiler,
            directory / "tampered.txt",
            "profile BinaryFILE SHA-256 differs",
        )

        tampered_profiler = directory / "tampered-profiler"
        tampered_profiler.write_bytes(profiler.read_bytes() + b"tampered\n")
        expect_failure(
            generator,
            profile,
            corpus,
            binaryfile,
            tampered_profiler,
            directory / "tampered-profiler.txt",
            "profile profiler SHA-256 differs",
        )

        false_profiler_claim = directory / "false-profiler.profile.txt"
        false_profiler_claim.write_text(
            clean_profile.replace(
                f"profiler_sha256={profiler_sha256}",
                f"profiler_sha256={'0' * 64}",
            ),
            encoding="ascii",
        )
        expect_failure(
            generator,
            false_profiler_claim,
            corpus,
            binaryfile,
            profiler,
            directory / "false-profiler-claim.txt",
            "profile profiler SHA-256 differs",
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
