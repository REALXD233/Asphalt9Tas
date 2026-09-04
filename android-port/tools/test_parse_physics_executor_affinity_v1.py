#!/usr/bin/env python3

from __future__ import annotations

import struct
import unittest

from parse_physics_executor_affinity_v1 import (
    ATOMIC_BRANCH_PATCH,
    EVENT,
    EVENT_SIZE,
    HEADER,
    HEADER_SIZE,
    INSTALLED,
    MAGIC,
    PASSIVE_BUILD,
    RUNTIME_ARMING_COMPILED,
    TOKEN_ZERO,
    VERSION,
    decode_report,
)


def report(tids=(7001, 7001, 7001), tokens=(16667, 0, 16666),
           installed=True) -> bytes:
    capacity = 8
    events = len(tids)
    times = tuple(1_000_000 + index * 100 for index in range(events))
    flags = (
        RUNTIME_ARMING_COMPILED | INSTALLED | ATOMIC_BRANCH_PATCH
        if installed else PASSIVE_BUILD
    )
    zero = sum(token == 0 for token in tokens)
    nonzero = events - zero
    header = HEADER.pack(
        MAGIC, VERSION, HEADER_SIZE, EVENT_SIZE, capacity, flags, 0,
        events, 0, times[0], times[-1], tids[0], tids[-1],
        sum(tids[index] != tids[index - 1] for index in range(1, events)),
        0, zero, nonzero, tokens[0], tokens[-1],
        0x700000000000, 0x7000038B74DC,
    )
    records = bytearray(capacity * EVENT_SIZE)
    for index, (tid, token, timestamp) in enumerate(zip(tids, tokens, times)):
        event_flags = TOKEN_ZERO if token == 0 else 0
        EVENT.pack_into(records, index * EVENT_SIZE, index, timestamp, token,
                        0x710000000000, 0x720000000000, tid, event_flags,
                        index + 1)
    return header + records


class ParserTests(unittest.TestCase):
    def test_valid_single_tid_installed(self):
        decoded = decode_report(report(), require_installed=True)
        self.assertTrue(decoded["unique_tid"])
        self.assertEqual(decoded["tids"], (7001,))

    def test_multi_tid_is_reported_not_hidden(self):
        decoded = decode_report(
            report(tids=(7001, 7002, 7001)), require_installed=True)
        self.assertFalse(decoded["unique_tid"])
        self.assertEqual(decoded["tids"], (7001, 7002))

    def test_uncommitted_event_rejected(self):
        blob = bytearray(report())
        commit_offset = HEADER_SIZE + 2 * EVENT_SIZE + 48
        struct.pack_into("<Q", blob, commit_offset, 0)
        with self.assertRaisesRegex(ValueError, "sequence/commit"):
            decode_report(bytes(blob), require_installed=True)

    def test_counter_mismatch_rejected(self):
        blob = bytearray(report())
        # zero_tokens is the Q at header offset 80.
        struct.pack_into("<Q", blob, 80, 99)
        with self.assertRaisesRegex(ValueError, "counters"):
            decode_report(bytes(blob), require_installed=True)

    def test_build_only_snapshot_rejected_as_live(self):
        with self.assertRaisesRegex(ValueError, "installed"):
            decode_report(report(installed=False), require_installed=True)

    def test_mixed_build_modes_rejected(self):
        blob = bytearray(report())
        # flags is the I at header offset 24.
        current = struct.unpack_from("<I", blob, 24)[0]
        struct.pack_into("<I", blob, 24, current | PASSIVE_BUILD)
        with self.assertRaisesRegex(ValueError, "exactly one build mode"):
            decode_report(bytes(blob), require_installed=True)


if __name__ == "__main__":
    unittest.main()
