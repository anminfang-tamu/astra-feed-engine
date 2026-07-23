#!/usr/bin/env python3
"""Focused synthetic-disassembly tests for the hot-path verifier."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts/verify_order_book_hot_path.sh"

REDESIGN_REQUIRED = (
    "BookManager::addOrder()",
    "BookManager::executeOrder()",
    "BookManager::cancelShares()",
    "BookManager::deleteOrder()",
    "BookManager::replaceOrder()",
    "OrderBook::addOrder()",
    "OrderBook::executeOrder()",
    "OrderBook::cancelShares()",
    "OrderBook::deleteOrder()",
    "OrderBook::replaceOrder()",
    "astra::book::OrderTable::reserve()",
    "astra::book::OrderTable::commit()",
    "astra::book::OrderTable::findFallbackState()",
    "astra::book::PriceLevelStore::add()",
    "astra::book::PriceLevelStore::reduceChecked()",
    "astra::book::PriceLevelStore::erase()",
    "astra::book::PriceLevelStore::move()",
    "astra::book::PriceLevelStore::nextWorse()",
    "astra::book::PriceLevelStore::topTen()",
    "ItchParser::handleMessage()",
    "ItchParser::handlePrevalidatedMessage()",
    "ItchParser::dispatchMessage()",
    "ItchParser::handleAdd()",
    "ItchParser::handleOrderExecuted()",
    "ItchParser::handleOrderExecutedWithPrice()",
    "ItchParser::applyExecution()",
    "ItchParser::handleCancel()",
    "ItchParser::handleDelete()",
    "ItchParser::handleReplace()",
    "ItchParser::applyBookResult()",
    "ItchParser::fail()",
    "ItchParser::applyBookFailure()",
    *(f"OrderBook::fixtureTraversal{ordinal}()" for ordinal in range(12)),
)

BRANCH5_REQUIRED = (
    "BookManager::addOrder()",
    "BookManager::trade()",
    "BookManager::cancelShares()",
    "BookManager::deleteOrder()",
    "BookManager::replaceOrder()",
    "OrderBook::addOrder()",
    "OrderBook::trade()",
    "OrderBook::cancelShares()",
    "OrderBook::deleteOrder()",
    "OrderBook::replaceOrder()",
    "OrderBook::addOrderIndexed()",
    "OrderBook::tradeIndexed()",
    "OrderBook::cancelSharesIndexed()",
    "OrderBook::deleteOrderIndexed()",
    "OrderBook::replaceOrderIndexed()",
    "OrderBook::removeFromLevel()",
    "OrderArena::allocate()",
    "OrderArena::release()",
    "OrderArena::at()",
    "OrderArena::isInUse()",
    "OrderArena::markFree()",
    "OrderArena::markInUse()",
    "LocalOrderRefMap::erase()",
    "LocalOrderRefMap::replaceKey()",
    "PriceLevelIndex::ensure()",
    "PriceLevelIndex::find()",
    "PriceLevelIndex::eraseEmpty()",
    "PriceLevelIndex::best()",
    "PriceLevelIndex::nextWorse()",
    "PriceLevelArena::allocateNode()",
    "PriceLevelArena::allocateLeaf()",
    "PriceLevelArena::allocateLevel()",
    "PriceLevelArena::releaseNode()",
    "PriceLevelArena::releaseLeaf()",
    "PriceLevelArena::releaseLevel()",
    "PriceLevelArena::level()",
    "PriceLevelArena::node()",
    "PriceLevelArena::leaf()",
    "ItchParser::handleMessage()",
    "ItchParser::handleAdd()",
    "ItchParser::handleExecution()",
    "ItchParser::handleCancel()",
    "ItchParser::handleDelete()",
    "ItchParser::handleReplace()",
)


class SyntheticDisassembly:
    def __init__(self, symbols: Iterable[str]) -> None:
        self.symbols = list(dict.fromkeys(symbols))
        self.instructions: dict[str, list[str]] = {
            symbol: [] for symbol in self.symbols
        }
        self.addresses = {
            symbol: 0x1000 + ordinal * 0x20
            for ordinal, symbol in enumerate(self.symbols)
        }

    def add_symbol(self, symbol: str) -> None:
        if symbol in self.addresses:
            return
        self.addresses[symbol] = 0x1000 + len(self.symbols) * 0x20
        self.symbols.append(symbol)
        self.instructions[symbol] = []

    def call(self, caller: str, target: str) -> None:
        target_address = self.addresses[target]
        self.instructions[caller].append(
            f"call {target_address:x} <{target}>"
        )

    def external_call(self, caller: str, target: str) -> None:
        self.instructions[caller].append(f"call f00000 <{target}>")

    def instruction(self, caller: str, instruction: str) -> None:
        self.instructions[caller].append(instruction)

    def render(self) -> str:
        lines: list[str] = []
        for symbol in self.symbols:
            address = self.addresses[symbol]
            lines.append(f"{address:016x} <{symbol}>:")
            body = self.instructions[symbol] or ["ret"]
            for offset, instruction in enumerate(body, start=1):
                lines.append(
                    f" {address + offset:x}: 90                   {instruction}"
                )
            lines.append("")
        return "\n".join(lines)


class VerifierResult:
    def __init__(
        self, result: subprocess.CompletedProcess[str], report: str | None
    ) -> None:
        self.returncode = result.returncode
        self.stdout = result.stdout
        self.stderr = result.stderr
        self.report = report


def invoke(
    schema: str,
    disassembly: SyntheticDisassembly,
    *,
    report: bool = False,
) -> VerifierResult:
    with tempfile.TemporaryDirectory(prefix="astra-hot-path-test-") as raw_root:
        root = Path(raw_root)
        binary = root / "fixture-binary"
        binary.write_bytes(b"synthetic executable\n")
        binary.chmod(0o755)

        disassembly_path = root / "objdump.txt"
        disassembly_path.write_text(disassembly.render(), encoding="utf-8")

        fake_bin = root / "bin"
        fake_bin.mkdir()
        fake_objdump = fake_bin / "objdump"
        fake_objdump.write_text(
            "#!/usr/bin/env python3\n"
            "import os\n"
            "from pathlib import Path\n"
            "print(Path(os.environ['ASTRA_TEST_OBJDUMP']).read_text(), end='')\n",
            encoding="utf-8",
        )
        fake_objdump.chmod(0o755)

        report_path = root / "report.txt"
        arguments = [str(VERIFIER), "--schema", schema, str(binary)]
        if report:
            arguments.append(str(report_path))
        environment = os.environ.copy()
        environment["PATH"] = f"{fake_bin}{os.pathsep}{environment['PATH']}"
        environment["ASTRA_TEST_OBJDUMP"] = str(disassembly_path)
        result = subprocess.run(
            arguments,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
        )
        report_text = (
            report_path.read_text(encoding="utf-8")
            if report_path.exists()
            else None
        )
        return VerifierResult(result, report_text)


class VerifyOrderBookHotPathTest(unittest.TestCase):
    def test_redesign_live_and_replay_entrypoints_are_in_report(self) -> None:
        fixture = SyntheticDisassembly(REDESIGN_REQUIRED)
        result = invoke("redesign_v1", fixture, report=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hot_path_verifier version=2 result=PASS", result.stdout)
        self.assertIsNotNone(result.report)
        assert result.report is not None
        self.assertIn("<ItchParser::handleMessage()>", result.report)
        self.assertIn("<ItchParser::handlePrevalidatedMessage()>", result.report)
        self.assertIn("<ItchParser::dispatchMessage()>", result.report)

    def test_missing_live_entrypoint_fails_even_when_a_call_names_it(self) -> None:
        fixture = SyntheticDisassembly(
            symbol
            for symbol in REDESIGN_REQUIRED
            if symbol != "ItchParser::handlePrevalidatedMessage()"
        )
        fixture.external_call(
            "ItchParser::handleMessage()",
            "ItchParser::handlePrevalidatedMessage()",
        )
        result = invoke("redesign_v1", fixture)
        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "required hot-path symbol is absent: "
            "ItchParser::handlePrevalidatedMessage(",
            result.stderr,
        )

    def test_missing_required_redesign_cold_boundary_fails_closed(self) -> None:
        fixture = SyntheticDisassembly(
            symbol
            for symbol in REDESIGN_REQUIRED
            if symbol != "ItchParser::applyBookFailure()"
        )
        result = invoke("redesign_v1", fixture)
        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "required hot-path symbol is absent: ItchParser::applyBookFailure(",
            result.stderr,
        )

    def test_branch5_contract_and_shared_parser_entrypoint_still_pass(self) -> None:
        fixture = SyntheticDisassembly(BRANCH5_REQUIRED)
        result = invoke("branch5_native_v1", fixture, report=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("version=2 result=PASS schema=branch5_native_v1", result.stdout)
        self.assertIsNotNone(result.report)
        assert result.report is not None
        self.assertIn("<ItchParser::handleMessage()>", result.report)

    def test_transitive_unmatched_project_helper_is_audited(self) -> None:
        fixture = SyntheticDisassembly(
            (*REDESIGN_REQUIRED, "OpaqueStageOne()", "OpaqueStageTwo()")
        )
        fixture.call("ItchParser::handleAdd()", "OpaqueStageOne()")
        fixture.call("OpaqueStageOne()", "OpaqueStageTwo()")
        fixture.external_call("OpaqueStageTwo()", "malloc@plt")
        result = invoke("redesign_v1", fixture)
        self.assertEqual(result.returncode, 2)
        self.assertIn("malloc@plt", result.stdout)
        self.assertIn("forbidden allocator", result.stderr)

    def test_unmatched_project_helper_is_in_success_report(self) -> None:
        fixture = SyntheticDisassembly((*REDESIGN_REQUIRED, "UnpatternedHelper()"))
        fixture.call("ItchParser::handleCancel()", "UnpatternedHelper()")
        result = invoke("redesign_v1", fixture, report=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNotNone(result.report)
        assert result.report is not None
        self.assertIn("<UnpatternedHelper()>", result.report)

    def test_unreachable_helper_remains_outside_mutation_closure(self) -> None:
        fixture = SyntheticDisassembly((*REDESIGN_REQUIRED, "UnreachableHelper()"))
        fixture.external_call("UnreachableHelper()", "malloc@plt")
        result = invoke("redesign_v1", fixture, report=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNotNone(result.report)
        assert result.report is not None
        self.assertNotIn("<UnreachableHelper()>", result.report)

    def test_named_redesign_cold_error_helpers_may_allocate(self) -> None:
        fixture = SyntheticDisassembly(REDESIGN_REQUIRED)
        fixture.external_call(
            "ItchParser::applyBookFailure()",
            "operator new(unsigned long)@plt",
        )
        fixture.external_call("ItchParser::fail()", "operator delete(void*)@plt")
        result = invoke("redesign_v1", fixture)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_direct_allocator_in_redesign_parser_root_fails(self) -> None:
        for parser_root in (
            "ItchParser::handleMessage()",
            "ItchParser::handleAdd()",
            "ItchParser::applyBookResult()",
        ):
            with self.subTest(parser_root=parser_root):
                fixture = SyntheticDisassembly(REDESIGN_REQUIRED)
                fixture.external_call(parser_root, "malloc@plt")
                result = invoke("redesign_v1", fixture)
                self.assertEqual(result.returncode, 2)
                self.assertIn("malloc@plt", result.stdout)
                self.assertIn("forbidden allocator", result.stderr)

    def test_unnamed_error_helper_does_not_inherit_cold_exception(self) -> None:
        fixture = SyntheticDisassembly(
            (*REDESIGN_REQUIRED, "ItchParser::otherFailure()")
        )
        fixture.call("ItchParser::applyBookResult()", "ItchParser::otherFailure()")
        fixture.external_call("ItchParser::otherFailure()", "malloc@plt")
        result = invoke("redesign_v1", fixture)
        self.assertEqual(result.returncode, 2)
        self.assertIn("malloc@plt", result.stdout)
        self.assertIn("forbidden allocator", result.stderr)

    def test_mapping_in_named_cold_helper_is_still_forbidden(self) -> None:
        fixture = SyntheticDisassembly(REDESIGN_REQUIRED)
        fixture.external_call("ItchParser::applyBookFailure()", "mmap@plt")
        result = invoke("redesign_v1", fixture)
        self.assertEqual(result.returncode, 2)
        self.assertIn("mmap@plt", result.stdout)
        self.assertIn("forbidden allocator", result.stderr)

    def test_inline_kernel_entry_in_transitive_helper_fails(self) -> None:
        for instruction in ("syscall", "svc #0"):
            with self.subTest(instruction=instruction):
                fixture = SyntheticDisassembly(
                    (*REDESIGN_REQUIRED, "RawKernelEntryHelper()")
                )
                fixture.call(
                    "ItchParser::handleDelete()", "RawKernelEntryHelper()"
                )
                fixture.instruction("RawKernelEntryHelper()", instruction)
                result = invoke("redesign_v1", fixture)
                self.assertEqual(result.returncode, 2)
                self.assertIn(instruction, result.stdout)
                self.assertIn("forbidden allocator/mapping/syscall", result.stderr)

    def test_x86_lock_prefix_detection_handles_operand_and_bare_forms(self) -> None:
        for instruction in (
            "lock addl $1, (%rax)",
            "data16 lock addl $1, (%rax)",
            "lock; addl $1, (%rax)",
            "lock",
        ):
            with self.subTest(instruction=instruction):
                fixture = SyntheticDisassembly(
                    (*REDESIGN_REQUIRED, "LockedOpaqueHelper()")
                )
                fixture.call("ItchParser::handleCancel()", "LockedOpaqueHelper()")
                fixture.instruction("LockedOpaqueHelper()", instruction)
                result = invoke("redesign_v1", fixture)
                self.assertEqual(result.returncode, 2)
                self.assertIn(instruction, result.stdout)
                self.assertIn("x86 lock-prefixed instruction", result.stderr)

        symbol_only = SyntheticDisassembly(
            (*REDESIGN_REQUIRED, "ProjectMutex::lock()")
        )
        symbol_only.call("ItchParser::handleCancel()", "ProjectMutex::lock()")
        result = invoke("redesign_v1", symbol_only)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_indirect_call_in_transitive_helper_fails(self) -> None:
        fixture = SyntheticDisassembly((*BRANCH5_REQUIRED, "NativeOpaqueHelper()"))
        fixture.call("ItchParser::handleExecution()", "NativeOpaqueHelper()")
        fixture.instruction("NativeOpaqueHelper()", "call *%rax")
        result = invoke("branch5_native_v1", fixture)
        self.assertEqual(result.returncode, 2)
        self.assertIn("call *%rax", result.stdout)
        self.assertIn("indirect call or tail call", result.stderr)

    def test_one_dispatch_jump_table_is_allowed_but_two_fail(self) -> None:
        allowed = SyntheticDisassembly(REDESIGN_REQUIRED)
        allowed.instruction("ItchParser::dispatchMessage()", "jmp *%rax")
        result = invoke("redesign_v1", allowed)
        self.assertEqual(result.returncode, 0, result.stderr)

        rejected = SyntheticDisassembly(REDESIGN_REQUIRED)
        rejected.instruction("ItchParser::dispatchMessage()", "jmp *%rax")
        rejected.instruction("ItchParser::dispatchMessage()", "jmp *%rcx")
        result = invoke("redesign_v1", rejected)
        self.assertEqual(result.returncode, 2)
        self.assertIn("indirect call or tail call", result.stderr)

    def test_branch5_two_historical_jump_tables_are_allowed(self) -> None:
        allowed = SyntheticDisassembly(BRANCH5_REQUIRED)
        allowed.instruction("ItchParser::handleMessage()", "jmp *%rax")
        allowed.instruction("ItchParser::handleMessage()", "jmp *%rcx")
        result = invoke("branch5_native_v1", allowed)
        self.assertEqual(result.returncode, 0, result.stderr)

        rejected = SyntheticDisassembly(BRANCH5_REQUIRED)
        rejected.instruction("ItchParser::handleMessage()", "jmp *%rax")
        rejected.instruction("ItchParser::handleMessage()", "jmp *%rcx")
        rejected.instruction("ItchParser::handleMessage()", "jmp *%rdx")
        result = invoke("branch5_native_v1", rejected)
        self.assertEqual(result.returncode, 2)
        self.assertIn("indirect call or tail call", result.stderr)


if __name__ == "__main__":
    unittest.main()
