#!/usr/bin/env python3

import struct
import tempfile
import unittest
from pathlib import Path

from parse_hwbp_worker_boundary_v1 import (
    EVENT,
    HEADER,
    HEADER_CLEAN,
    HEADER_TARGET_VERIFIED,
    HIT_ANGULAR,
    HIT_LINEAR,
    HIT_PHASE,
    HIT_POSE,
    MAGIC,
    VERSION,
    parse_trace,
)


class WorkerBoundaryParserTest(unittest.TestCase):
    def make_trace(self, outside_write: bool) -> Path:
        events = []
        sequence = 0
        bits = struct.unpack("<I", struct.pack("<f", 2.5))[0]

        def add(flags: int, active: int, rip: int = 0x10001000) -> None:
            nonlocal sequence
            events.append(
                EVENT.pack(
                    sequence,
                    2_000_000 + sequence * 1_000,
                    222,
                    flags,
                    rip,
                    active,
                    bits,
                    bits,
                    bits,
                    1,
                )
            )
            sequence += 1

        for cycle in range(20):
            add(HIT_PHASE, 1)
            add(HIT_LINEAR, 1, 0x10002000)
            add(HIT_ANGULAR, 1, 0x10003000)
            add(HIT_POSE, 1, 0x10004000)
            add(HIT_PHASE, 0)
            if outside_write and cycle == 0:
                add(HIT_LINEAR, 0, 0x10002000)

        counts = {flag: 0 for flag in (HIT_PHASE, HIT_LINEAR, HIT_ANGULAR, HIT_POSE)}
        for packed in events:
            flags = EVENT.unpack(packed)[3]
            for flag in counts:
                counts[flag] += int(bool(flags & flag))
        header = HEADER.pack(
            MAGIC,
            VERSION,
            HEADER.size,
            EVENT.size,
            HEADER_CLEAN | HEADER_TARGET_VERIFIED,
            11,
            0x70000000,
            0x60000000,
            0x61000000,
            0x61000150,
            0x61000160,
            0x61000040,
            2_000_000,
            len(events),
            counts[HIT_PHASE],
            counts[HIT_LINEAR],
            counts[HIT_ANGULAR],
            counts[HIT_POSE],
            0,
            0,
            0,
            222,
            1,
            1,
            0,
        )
        handle = tempfile.NamedTemporaryFile(delete=False, suffix=".a9wbp1")
        handle.write(header)
        handle.write(b"".join(events))
        handle.close()
        self.addCleanup(Path(handle.name).unlink, missing_ok=True)
        return Path(handle.name)

    def test_accepts_complete_worker_scope(self) -> None:
        report = parse_trace(self.make_trace(False), minimum_pairs=20)
        self.assertTrue(report["integrity"]["transport_clean"])
        self.assertTrue(report["assessment"]["boundary_supported"])

    def test_rejects_write_after_worker_exit(self) -> None:
        report = parse_trace(self.make_trace(True), minimum_pairs=20)
        self.assertEqual(
            report["assessment"]["inactive_state_writes"]["native_linear"], 1
        )
        self.assertFalse(report["assessment"]["boundary_supported"])


if __name__ == "__main__":
    unittest.main()
