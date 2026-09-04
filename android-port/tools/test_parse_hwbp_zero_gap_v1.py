#!/usr/bin/env python3

import struct
import tempfile
import unittest
from pathlib import Path

from parse_hwbp_zero_gap_v1 import (
    EVENT,
    HEADER,
    HEADER_CLEAN,
    HEADER_TARGET_VERIFIED,
    HIT_ACCUMULATOR,
    HIT_ANGULAR,
    HIT_LINEAR,
    HIT_POSE,
    MAGIC,
    VERSION,
    parse_trace,
)


class ZeroGapParserTest(unittest.TestCase):
    def make_trace(self, zero_gap_violation: bool) -> Path:
        events = []
        sequence = 0

        def add(flags: int, accumulator: int, rip: int = 0x70000100) -> None:
            nonlocal sequence
            bits = struct.unpack("<I", struct.pack("<f", 1.25))[0]
            events.append(
                EVENT.pack(
                    sequence,
                    1_000_000 + sequence * 1_000,
                    101 if flags & HIT_ACCUMULATOR else 202,
                    flags,
                    rip,
                    accumulator,
                    bits,
                    bits,
                    bits,
                    1,
                )
            )
            sequence += 1

        for cycle in range(20):
            add(HIT_ACCUMULATOR, 1)
            add(HIT_LINEAR, 1, 0x71000010)
            add(HIT_ANGULAR, 1, 0x71000020)
            add(HIT_POSE, 1, 0x70001000)
            add(HIT_ACCUMULATOR, 0)
            if zero_gap_violation and cycle == 0:
                add(HIT_LINEAR, 0, 0x71000010)
            add(HIT_ACCUMULATOR, 1)

        hit_counts = {
            HIT_ACCUMULATOR: 0,
            HIT_LINEAR: 0,
            HIT_ANGULAR: 0,
            HIT_POSE: 0,
        }
        for packed in events:
            flags = EVENT.unpack(packed)[3]
            for flag in hit_counts:
                hit_counts[flag] += int(bool(flags & flag))

        header = HEADER.pack(
            MAGIC,
            VERSION,
            HEADER.size,
            EVENT.size,
            HEADER_CLEAN | HEADER_TARGET_VERIFIED,
            42,
            0x70000000,
            0x60000000,
            0x60000040,
            0x61000000,
            0x61000160,
            0x61000170,
            0x62000000,
            0x6200000C,
            1_000_000,
            len(events),
            hit_counts[HIT_ACCUMULATOR],
            hit_counts[HIT_LINEAR],
            hit_counts[HIT_ANGULAR],
            hit_counts[HIT_POSE],
            0,
            0,
            0,
            2,
            2,
        )
        handle = tempfile.NamedTemporaryFile(delete=False, suffix=".a9zgb1")
        handle.write(header)
        handle.write(b"".join(events))
        handle.close()
        self.addCleanup(Path(handle.name).unlink, missing_ok=True)
        return Path(handle.name)

    def test_accepts_clean_quiescent_trace(self) -> None:
        report = parse_trace(self.make_trace(False), minimum_cycles=20)
        self.assertTrue(report["integrity"]["transport_clean"])
        self.assertEqual(report["phase_analysis"]["closed_zero_gaps"], 20)
        self.assertTrue(report["assessment"]["quiescence_supported"])

    def test_rejects_state_write_inside_zero_gap(self) -> None:
        report = parse_trace(self.make_trace(True), minimum_cycles=20)
        self.assertEqual(
            report["phase_analysis"]["zero_gap_writes"]["native_linear"], 1
        )
        self.assertFalse(report["assessment"]["quiescence_supported"])


if __name__ == "__main__":
    unittest.main()
