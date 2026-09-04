#!/usr/bin/env python3
"""Small offline regression tests for analyze_hwbp_events_v5.py."""

import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from analyze_hwbp_events_v5 import EventRecord, summarize  # noqa: E402


class SummarizeTests(unittest.TestCase):
    def test_strict_alternating_pairs(self) -> None:
        events = [
            EventRecord(0, 1_000_000, 10, 1, 0x100, 0, 0, 1),
            EventRecord(1, 1_100_000, 10, 2, 0x200, 0, 0, 1),
            EventRecord(2, 18_000_000, 10, 1, 0x100, 0, 0, 1),
            EventRecord(3, 18_100_000, 10, 2, 0x200, 0, 0, 1),
        ]
        result = summarize({"flags": 5, "c98_hits": 2, "c9c_hits": 2}, events)
        self.assertEqual(result["strict_pairs"], 2)
        self.assertEqual(result["pair_count"], 2)
        self.assertEqual(result["dominant_pair"], (("C98", "C9C"), 2))
        self.assertTrue(result["target_signature_verified"])
        self.assertEqual(result["inferred_order"], ("C98", "C9C"))
        self.assertTrue(result["pairing_confident"])
        self.assertEqual(len(result["inferred_tick_pairs"]), 2)

    def test_detects_event_sequence_not_tick_sequence(self) -> None:
        events = [
            EventRecord(0, 1, 10, 1, 0, 0, 0, 1),
            EventRecord(1, 2, 10, 2, 0, 0, 0, 1),
            EventRecord(2, 3, 10, 2, 0, 0, 0, 1),
            EventRecord(3, 4, 10, 1, 0, 0, 0, 1),
        ]
        result = summarize({"flags": 1, "c98_hits": 2, "c9c_hits": 2}, events)
        self.assertEqual(result["strict_pairs"], 2)
        self.assertEqual(len(result["pair_orders"]), 2)

    def test_sequence_gap_is_reported(self) -> None:
        events = [
            EventRecord(0, 1, 10, 1, 0, 0, 0, 1),
            EventRecord(2, 2, 10, 2, 0, 0, 0, 1),
        ]
        result = summarize({"flags": 1, "c98_hits": 1, "c9c_hits": 1}, events)
        self.assertEqual(result["sequence_errors"], [(1, 2)])

    def test_timing_pairing_trims_leading_partial_tick(self) -> None:
        events = [
            EventRecord(0, 100_000, 10, 2, 0, 0, 0, 1),
            EventRecord(1, 17_000_000, 10, 1, 0, 0, 0, 1),
            EventRecord(2, 17_100_000, 10, 2, 0, 0, 0, 1),
            EventRecord(3, 34_000_000, 10, 1, 0, 0, 0, 1),
            EventRecord(4, 34_100_000, 10, 2, 0, 0, 0, 1),
        ]
        result = summarize({"flags": 5, "c98_hits": 2, "c9c_hits": 3}, events)
        self.assertEqual(result["inferred_order"], ("C98", "C9C"))
        self.assertEqual(result["unpaired_events"], [0])
        self.assertEqual(len(result["inferred_tick_pairs"]), 2)

    def test_rejects_finite_out_of_range_lifecycle_value(self) -> None:
        huge_bits = struct.unpack("<I", struct.pack("<f", 887_289_344.0))[0]
        events = [EventRecord(0, 1, 10, 3, 0, huge_bits, 0, 1)]
        result = summarize(
            {"flags": 5, "c98_hits": 1, "c9c_hits": 1}, events
        )
        self.assertEqual(result["invalid_floats"], [0])


if __name__ == "__main__":
    unittest.main()
