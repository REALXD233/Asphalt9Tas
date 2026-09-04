#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("parse_hwbp_worker_stack_scope_v1.py")
SPEC = importlib.util.spec_from_file_location("worker_stack_scope_parser", MODULE_PATH)
assert SPEC and SPEC.loader
PARSER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PARSER)


class WorkerStackScopeParserTest(unittest.TestCase):
    def make_trace(
        self,
        path: Path,
        *,
        missing_marker_at: int | None = None,
        duplicate_marker_at: int | None = None,
    ) -> None:
        base = 0x700000000000
        phase_returns = tuple(
            base + rva for rva in PARSER.EXPECTED_PHASE_RETURN_RVAS
        )
        events = []
        flags = (
            PARSER.HEADER_CLEAN | PARSER.HEADER_TARGET_VERIFIED
        )
        per_field = 24
        state_flags = (
            PARSER.HIT_LINEAR,
            PARSER.HIT_ANGULAR,
            PARSER.HIT_POSE,
        )
        for sequence in range(per_field * len(state_flags)):
            event_flag = state_flags[sequence // per_field]
            stack = [0] * 32
            stack[2] = phase_returns[sequence % len(phase_returns)]
            if sequence == missing_marker_at:
                stack[2] = 0
            if sequence == duplicate_marker_at:
                stack[3] = phase_returns[1]
            events.append(
                PARSER.EVENT.pack(
                    sequence,
                    1_000_000 + sequence,
                    4321,
                    event_flag,
                    0x101000 + sequence,
                    0x200000,
                    struct.unpack("<I", struct.pack("<f", 1.0))[0],
                    struct.unpack("<I", struct.pack("<f", 2.0))[0],
                    struct.unpack("<I", struct.pack("<f", 3.0))[0],
                    1,
                    1,
                    0,
                    *stack,
                )
            )
        header = PARSER.HEADER.pack(
            PARSER.MAGIC,
            PARSER.VERSION,
            PARSER.HEADER.size,
            PARSER.EVENT.size,
            flags,
            1234,
            base,
            0x500000,
            0x500150,
            0x500160,
            0x50004C,
            *phase_returns,
            900_000,
            len(events),
            per_field,
            per_field,
            per_field,
            0,
            0,
            0,
            0,
            4321,
            1,
            1,
            0,
        )
        path.write_bytes(header + b"".join(events))

    def test_accepts_complete_scoped_trace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "accepted.bin"
            self.make_trace(path)
            report = PARSER.parse_trace(path, minimum_hits_per_field=20)
            self.assertTrue(report["structurally_valid"])
            self.assertTrue(report["integrity"]["transport_clean"])
            self.assertTrue(report["assessment"]["all_writes_scoped"])
            self.assertTrue(report["assessment"]["boundary_supported"])

    def test_rejects_one_write_without_phase_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "missing.bin"
            self.make_trace(path, missing_marker_at=5)
            report = PARSER.parse_trace(path, minimum_hits_per_field=20)
            self.assertFalse(report["assessment"]["all_writes_scoped"])
            self.assertFalse(report["assessment"]["boundary_supported"])
            self.assertEqual(
                report["state_writes"]["native_linear"]
                      ["missing_phase_marker_hits"],
                1,
            )

    def test_rejects_ambiguous_phase_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ambiguous.bin"
            self.make_trace(path, duplicate_marker_at=30)
            report = PARSER.parse_trace(path, minimum_hits_per_field=20)
            self.assertFalse(report["assessment"]["all_writes_scoped"])
            self.assertFalse(report["assessment"]["boundary_supported"])
            self.assertEqual(
                report["state_writes"]["native_angular"]
                      ["ambiguous_phase_marker_hits"],
                1,
            )


if __name__ == "__main__":
    unittest.main()
