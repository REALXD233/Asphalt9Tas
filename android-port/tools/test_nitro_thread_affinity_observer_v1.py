#!/usr/bin/env python3
"""Offline policy and parser tests for the Nitro thread-affinity probe."""

from __future__ import annotations

import pathlib
import struct
import unittest

from parse_nitro_thread_affinity_report_v1 import (
    CAPTURED_DELTA,
    CAPTURED_NITRO,
    CLEAN_DETACH,
    EVENT_SIZE,
    HEADER_SIZE,
    HIT_ACTIVE,
    HIT_DELTA,
    HIT_ENCRYPTED,
    HIT_MODE,
    MAGIC,
    TARGET_VERIFIED,
    VERSION,
    _EVENT_PREFIX,
    _HEADER_PREFIX,
    _SNAPSHOT,
    decode_report,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_nitro_thread_affinity_observer_v1.cpp").read_text(
    encoding="utf-8"
)
RUNNER = (ROOT / "run-nitro-thread-affinity-observer-v1.ps1").read_text(
    encoding="utf-8"
)


def synthetic_report(*, same_tid: bool = True) -> bytes:
    snapshot = _SNAPSHOT.pack(
        0, 0, b"\x00\x01\x01\x01\x01", 0, 123, 456, 1, 2, 3
    )
    flags = TARGET_VERIFIED | CLEAN_DETACH | CAPTURED_DELTA | CAPTURED_NITRO
    events = [
        _EVENT_PREFIX.pack(0, 1000, 41, HIT_DELTA, 0x7000, 16667)
        + snapshot
        + struct.pack("<I", 1),
        _EVENT_PREFIX.pack(
            1, 1100, 41 if same_tid else 52,
            HIT_ACTIVE | HIT_MODE | HIT_ENCRYPTED, 0x7100, 16667
        )
        + snapshot
        + struct.pack("<I", 1),
    ]
    header = _HEADER_PREFIX.pack(
        MAGIC, VERSION, HEADER_SIZE, EVENT_SIZE, flags,
        100, 0x100000, 0x200000, 0x200150, 0x300000,
        0x300CB8, 0x300CC0, 900, len(events), 1, 1, 1, 1,
        0, 0, 0, 0, 10, 10,
    )
    return header + snapshot + snapshot + b"".join(events)


class NitroThreadAffinityObserverTests(unittest.TestCase):
    def test_source_is_observation_only(self) -> None:
        self.assertIn("O_RDONLY | O_CLOEXEC", SOURCE)
        self.assertIn("write_scope=debug-registers-only", SOURCE)
        for forbidden in (
            "WriteExactVerified",
            "PTRACE_POKEDATA",
            "kServiceActivateRva)(",
            "input keyevent",
        ):
            self.assertNotIn(forbidden, SOURCE)

    def test_four_watchpoints_cover_delta_and_nitro_state(self) -> None:
        for evidence in (
            "kAccumulatorOffset",
            "kActiveOffset",
            "kModeOffset",
            "kEncryptedOffset",
            "NitroAffinityDr7",
        ):
            self.assertIn(evidence, SOURCE)

    def test_runner_requires_manual_input_and_never_sends_it(self) -> None:
        for gate in (
            "AcknowledgeNaturallyRunningRace",
            "AcknowledgeExactlyOneManualSpacePress",
            "AcknowledgeNoPausedAttach",
            "AcknowledgeGuestMemoryReadOnly",
            "AcknowledgeDebugRegistersOnly",
            "AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(gate, RUNNER)
        lowered = RUNNER.lower()
        for forbidden in ("input keyevent", "input tap", "force-stop", "am start"):
            self.assertNotIn(forbidden, lowered)

    def test_parser_reports_same_writer_without_forcing_it(self) -> None:
        same = decode_report(synthetic_report(same_tid=True))
        different = decode_report(synthetic_report(same_tid=False))
        self.assertTrue(same["nitro_tids_are_delta_writers"])
        self.assertFalse(different["nitro_tids_are_delta_writers"])

    def test_parser_rejects_missing_nitro_capture(self) -> None:
        report = bytearray(synthetic_report())
        fields = list(_HEADER_PREFIX.unpack_from(report))
        fields[4] &= ~CAPTURED_NITRO
        report[: _HEADER_PREFIX.size] = _HEADER_PREFIX.pack(*fields)
        with self.assertRaisesRegex(ValueError, "capture flags"):
            decode_report(bytes(report))

    def test_parser_rejects_unclean_or_unreadable_event(self) -> None:
        report = bytearray(synthetic_report())
        fields = list(_HEADER_PREFIX.unpack_from(report))
        fields[4] &= ~CLEAN_DETACH
        report[: _HEADER_PREFIX.size] = _HEADER_PREFIX.pack(*fields)
        with self.assertRaisesRegex(ValueError, "capture flags"):
            decode_report(bytes(report))
        report = bytearray(synthetic_report())
        struct.pack_into("<I", report, HEADER_SIZE + EVENT_SIZE - 4, 0)
        with self.assertRaisesRegex(ValueError, "event read failed"):
            decode_report(bytes(report))


if __name__ == "__main__":
    unittest.main()
