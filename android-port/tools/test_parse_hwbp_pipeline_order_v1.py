#!/usr/bin/env python3

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from parse_hwbp_pipeline_order_v1 import (
    CLEAN,
    EVENT,
    HEADER,
    HIT_ACCUMULATOR,
    HIT_CALLBACK_FLAGS,
    HIT_COMPLETION,
    HIT_F64,
    MAGIC,
    TARGET_VERIFIED,
    VERSION,
    assess,
    read_trace,
)


def float_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def build_trace(event_specs: list[tuple[int, int, int]]) -> bytes:
    # spec: flags, callback_flags, tid
    counts = [
        sum(bool(flags & bit) for flags, _, _ in event_specs)
        for bit in (HIT_COMPLETION, HIT_CALLBACK_FLAGS, HIT_F64, HIT_ACCUMULATOR)
    ]
    qwords = [
        1234, 0x70000000, 0x71000000, 0x710001D0,
        0x710001A0, 0x72000000, 0x72000F64, 0x73000000,
        0x74000000, 0x74000188, 1000, len(event_specs),
        *counts, 0, 0, 0, 0,
    ]
    blob = bytearray(
        HEADER.pack(
            MAGIC, VERSION, HEADER.size, EVENT.size,
            CLEAN | TARGET_VERIFIED, *qwords, 2, 2,
        )
    )
    for sequence, (flags, callback_flags, tid) in enumerate(event_specs):
        blob += EVENT.pack(
            sequence, 1000 + sequence, tid, flags, 0x7FFF0000,
            sequence, callback_flags, 0, float_bits(1.5),
            float_bits(0.01), 1, 0, 0,
        )
    return bytes(blob)


def good_cycle(owner_tid: int = 10, worker_tid: int = 11):
    return [
        (HIT_COMPLETION, 0, owner_tid),
        (HIT_CALLBACK_FLAGS, 1, owner_tid),
        (HIT_F64, 1, owner_tid),
        (HIT_CALLBACK_FLAGS, 0, owner_tid),
        (HIT_CALLBACK_FLAGS, 0, owner_tid),
        (HIT_ACCUMULATOR, 0, worker_tid),
    ]


class PipelineParserTests(unittest.TestCase):
    def parse(self, specs):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.bin"
            path.write_bytes(build_trace(specs))
            return read_trace(path)

    def test_accepts_ordered_cycles(self) -> None:
        header, events = self.parse(good_cycle() + good_cycle())
        supported, problems, stats = assess(header, events, 2)
        self.assertTrue(supported, problems)
        self.assertEqual(stats["cycles"], 2)

    def test_rejects_next_worker_before_callback_close(self) -> None:
        bad = good_cycle()
        bad.insert(3, (HIT_ACCUMULATOR, 1, 11))
        header, events = self.parse(bad)
        supported, problems, _ = assess(header, events, 1)
        self.assertFalse(supported)
        self.assertTrue(any("accumulator write" in problem for problem in problems))

    def test_rejects_f64_outside_callback(self) -> None:
        bad = good_cycle()
        bad[1], bad[2] = bad[2], bad[1]
        header, events = self.parse(bad)
        supported, problems, _ = assess(header, events, 1)
        self.assertFalse(supported)
        self.assertTrue(any("F64 publication" in problem for problem in problems))

    def test_rejects_unclean_transport(self) -> None:
        blob = bytearray(build_trace(good_cycle()))
        struct.pack_into("<I", blob, 20, TARGET_VERIFIED)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.bin"
            path.write_bytes(blob)
            header, events = read_trace(path)
        supported, problems, _ = assess(header, events, 1)
        self.assertFalse(supported)
        self.assertIn("transport did not finish cleanly", problems)


if __name__ == "__main__":
    unittest.main()
