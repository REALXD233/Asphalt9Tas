#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "parser", HERE / "parse_hwbp_executor_stack_affinity_v1.py"
)
assert SPEC and SPEC.loader
PARSER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PARSER)


class ExecutorStackAffinityParserTests(unittest.TestCase):
    def make_trace(
        self,
        path: Path,
        *,
        missing_executor_at: int | None = None,
        duplicate_executor_at: int | None = None,
        duplicate_phase_at: int | None = None,
        wrong_tid_at: int | None = None,
    ) -> None:
        base = 0x700000000000
        tid = 77
        phase_returns = tuple(
            base + rva for rva in PARSER.EXPECTED_PHASE_RETURN_RVAS
        )
        executor_return = base + PARSER.EXPECTED_EXECUTOR_RETURN_RVA
        events = []
        for sequence in range(24):
            stack = [0] * PARSER.STACK_WORDS
            stack[80] = executor_return
            # Model the live body+0x40 watchpoint: 20 integrated-state
            # publications are mixed with four adjacent copy/sync writers.
            if sequence < 20:
                stack[3] = phase_returns[sequence % len(phase_returns)]
            if sequence == missing_executor_at:
                stack[80] = 0
            if sequence == duplicate_executor_at:
                stack[81] = executor_return
            if sequence == duplicate_phase_at:
                stack[4] = phase_returns[(sequence + 1) % len(phase_returns)]
            event = PARSER.EVENT.pack(
                sequence,
                1_000_000 + sequence,
                tid + (1 if sequence == wrong_tid_at else 0),
                PARSER.HIT_POSE,
                0x12340000,
                0x56780000,
                0,
                0,
                struct.unpack("<I", struct.pack("<f", 1.0))[0],
                1,
                1,
                0,
                *stack,
            )
            events.append(event)
        header = PARSER.HEADER.pack(
            PARSER.MAGIC,
            PARSER.VERSION,
            PARSER.HEADER.size,
            PARSER.EVENT.size,
            PARSER.HEADER_CLEAN | PARSER.HEADER_TARGET_VERIFIED,
            100,
            base,
            0x710000000000,
            0x710000000150,
            0x710000000160,
            0x710000000040,
            *phase_returns,
            900_000,
            len(events),
            0,
            0,
            len(events),
            0,
            0,
            0,
            0,
            tid,
            1,
            1,
            0,
        )
        path.write_bytes(header + b"".join(events))

    def test_accepts_complete_affinity_trace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ok.bin"
            self.make_trace(path)
            report = PARSER.parse_trace(path)
            self.assertTrue(report["assessment"]["executor_affinity_supported"])
            self.assertEqual(report["evidence"]["phase_qualified_hits"], 20)
            self.assertEqual(report["evidence"]["non_phase_writer_hits"], 4)

    def test_rejects_missing_executor_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "missing.bin"
            self.make_trace(path, missing_executor_at=4)
            report = PARSER.parse_trace(path)
            self.assertFalse(report["assessment"]["executor_affinity_supported"])
            self.assertEqual(
                report["evidence"]["qualified_missing_executor_marker_hits"], 1
            )

    def test_rejects_duplicate_executor_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.bin"
            self.make_trace(path, duplicate_executor_at=7)
            report = PARSER.parse_trace(path)
            self.assertFalse(report["assessment"]["executor_affinity_supported"])
            self.assertEqual(
                report["evidence"]["qualified_ambiguous_executor_marker_hits"], 1
            )

    def test_rejects_non_phase_writer_without_executor_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "non-phase-missing-executor.bin"
            self.make_trace(path, missing_executor_at=22)
            report = PARSER.parse_trace(path)
            self.assertFalse(report["assessment"]["executor_affinity_supported"])
            self.assertEqual(
                report["evidence"]["all_event_missing_executor_marker_hits"], 1
            )

    def test_rejects_duplicate_phase_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate-phase.bin"
            self.make_trace(path, duplicate_phase_at=8)
            report = PARSER.parse_trace(path)
            self.assertFalse(report["assessment"]["executor_affinity_supported"])
            self.assertEqual(report["evidence"]["ambiguous_phase_marker_hits"], 1)

    def test_rejects_wrong_tid(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wrong-tid.bin"
            self.make_trace(path, wrong_tid_at=9)
            report = PARSER.parse_trace(path)
            self.assertFalse(report["assessment"]["executor_affinity_supported"])
            self.assertEqual(report["integrity"]["wrong_tid_events"], 1)


if __name__ == "__main__":
    unittest.main()
