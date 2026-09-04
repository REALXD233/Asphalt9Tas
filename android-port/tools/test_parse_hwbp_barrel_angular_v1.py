#!/usr/bin/env python3
"""Synthetic positive/negative tests for the barrel/angular parser."""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from parse_hwbp_barrel_angular_v1 import (
    CLEAN,
    EVENT,
    HEADER,
    HIT_AUX,
    HIT_HEAD,
    HIT_TAIL,
    MAGIC,
    TARGET_VERIFIED,
    VERSION,
    parse_trace,
)


BASE = 0x7000000000
NATIVE = 0x7100000000
ANGULAR = NATIVE + 0x160
CANDIDATE = BASE + 0x369E45C
WORKER = (BASE + 0x5E080A0, BASE + 0x5E080AC, BASE + 0x5E080B8)


def event(sequence: int, flags: int, stack: tuple[int, ...]) -> bytes:
    words = list(stack) + [0] * (32 - len(stack))
    return EVENT.pack(
        sequence,
        1000 + sequence,
        1234,
        flags,
        0x7F000000 + sequence,
        0x7200000000,
        1,
        2,
        3,
        0,
        1,
        1,
        *words,
    )


def trace(events: list[bytes], *, clean: bool = True) -> bytes:
    flags = TARGET_VERIFIED | (CLEAN if clean else 0)
    decoded = [EVENT.unpack(item) for item in events]
    head = sum(bool(item[3] & HIT_HEAD) for item in decoded)
    tail = sum(bool(item[3] & HIT_TAIL) for item in decoded)
    aux = sum(bool(item[3] & HIT_AUX) for item in decoded)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER.size,
        EVENT.size,
        flags,
        4321,
        BASE,
        NATIVE,
        ANGULAR,
        ANGULAR + 8,
        ANGULAR + 12,
        CANDIDATE,
        *WORKER,
        900,
        len(events),
        head,
        tail,
        aux,
        0,
        0,
        0,
        0,
        0,
        8,
        8,
    )
    return header + b"".join(events)


class BarrelAngularParserTests(unittest.TestCase):
    def parse_blob(self, blob: bytes, require_candidate: bool = False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.bin"
            path.write_bytes(blob)
            return parse_trace(path, require_candidate)

    def test_accepts_complete_candidate_sequence(self) -> None:
        summary = self.parse_blob(
            trace(
                [
                    event(0, HIT_HEAD, (CANDIDATE,)),
                    event(1, HIT_TAIL, (CANDIDATE,)),
                    event(2, HIT_AUX, (CANDIDATE,)),
                ]
            ),
            require_candidate=True,
        )
        self.assertEqual(summary.candidate_events, 3)
        self.assertEqual(summary.candidate_aux_events, 1)

    def test_worker_only_trace_passes_without_candidate_requirement(self) -> None:
        summary = self.parse_blob(trace([event(0, HIT_HEAD, (WORKER[2],))]))
        self.assertEqual(summary.worker_events, 1)

    def test_rejects_worker_only_when_candidate_is_required(self) -> None:
        with self.assertRaisesRegex(ValueError, "no complete"):
            self.parse_blob(
                trace([event(0, HIT_AUX, (WORKER[2],))]),
                require_candidate=True,
            )

    def test_rejects_ambiguous_candidate_and_worker_stack(self) -> None:
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.parse_blob(trace([event(0, HIT_AUX, (CANDIDATE, WORKER[0]))]))

    def test_rejects_unclean_trace(self) -> None:
        with self.assertRaisesRegex(ValueError, "detach cleanly"):
            self.parse_blob(trace([event(0, HIT_AUX, (CANDIDATE,))], clean=False))


if __name__ == "__main__":
    unittest.main()
