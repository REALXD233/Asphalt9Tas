#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import pathlib
import struct
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "phase_gate_validator", TOOLS / "validate_vehicle_camera_phase_gate_v1.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)
import analyze_vehicle_camera_phase_events_v1 as analyzer


def valid_report() -> bytes:
    frames = 360
    events = []
    sequence = 0
    corrected = 0
    for frame in range(frames):
        events.append(analyzer.EVENT.pack(
            sequence, sequence + 1, 1, 1, 10, frame, 0xFFFFFFFF, 0,
            frame, frame, 0, 0, 0, 0))
        sequence += 1
        events.append(analyzer.EVENT.pack(
            sequence, sequence + 1, 4, 0xE3, 20, frame, frame, 0,
            0, 0, frame, frame, 0, 0))
        sequence += 1
        flags = 0x13 if frame % 2 else 0x19
        if frame % 2 == 0:
            corrected += 1
        events.append(analyzer.EVENT.pack(
            sequence, sequence + 1, 2, flags, 10, frame, 0xFFFFFFFF, 0,
            frame, frame, 0, 0, 0, 0))
        sequence += 1
    report = bytearray(MODULE.REPORT_SIZE)
    report[:8] = b"A9VCPTR1"
    struct.pack_into("<IIII", report, 8, 1, MODULE.REPORT_SIZE, 3,
                     MODULE.REQUIRED_FLAGS)
    struct.pack_into("<II", report, 136, 4096, len(events))
    evidence = MODULE.EVIDENCE_PREFIX.pack(
        b"A9VCPE1\0", 1, 192, len(events), len(events), 0,
        frames, frames, frames, 0, 0, frames, frames, corrected,
        frames, 1, 20, 0, 0x1000, 0x2000, 0x3000)
    report[MODULE.EVIDENCE_OFFSET:
           MODULE.EVIDENCE_OFFSET + len(evidence)] = evidence
    return bytes(report) + b"".join(events)


class PhaseGateValidatorTests(unittest.TestCase):
    def test_accepts_complete_synthetic_gate(self) -> None:
        result = MODULE.validate(valid_report())
        self.assertEqual(result["events"], 1080)
        self.assertEqual(result["camera"], 360)
        self.assertEqual(result["corrected"], 180)
        self.assertEqual(result["camera_inside"], 360)

    def test_rejects_dropped_event_receipt(self) -> None:
        data = bytearray(valid_report())
        struct.pack_into("<Q", data, MODULE.EVIDENCE_OFFSET + 32, 1)
        with self.assertRaisesRegex(ValueError, "failure, recursion, or overflow"):
            MODULE.validate(bytes(data))


if __name__ == "__main__":
    unittest.main()
