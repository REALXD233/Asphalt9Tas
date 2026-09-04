#!/usr/bin/env python3
"""Strict codec tests for A9NPA1 natural pre-roll anchors."""

from __future__ import annotations

import dataclasses
import struct
import tempfile
import unittest
from pathlib import Path

from natural_preroll_anchor_v1 import (
    BOUND_FLAGS,
    SIZE,
    UNBOUND_FLAGS,
    NaturalPrerollAnchorV1,
    anchor_matches,
    bind_anchor,
    decode_anchor,
    encode_anchor,
    verify_source_binding,
)
from synchronized_tick_recording_v1 import decode_sync_report
from unified_tick_recording_v1 import SUPPORTED_BUILD_ID, decode_recording


ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "evidence" / "a9tas_autoesc_aligned_source_countdown3_run3_20260817.a9usr1"
RECORDING = ROOT / "evidence" / "a9tas_autoesc_aligned_source_countdown3_run3_20260817.a9utk1"


def make_unbound() -> NaturalPrerollAnchorV1:
    report = decode_sync_report(REPORT.read_bytes())
    fixed_interval_us, frames = decode_recording(RECORDING.read_bytes())
    audit = report.frames[0]
    # Tests use a certified source frame as a structurally valid stand-in for
    # the future preceding warmup cycle. Binding still requires frame0 exactness.
    return NaturalPrerollAnchorV1(
        flags=UNBOUND_FLAGS,
        fixed_interval_us=fixed_interval_us,
        frame_count=len(frames),
        build_id=SUPPORTED_BUILD_ID,
        recording_sha256=bytes(32),
        report_sha256=bytes(32),
        source_pid=report.pid,
        source_library_base=report.library_base,
        source_main_object=report.main_object,
        source_final_owner=report.final_owner,
        source_physics_context=report.physics_context,
        source_native_body=report.native_body,
        cycle_tid=audit.cycle_tid,
        commit_tid=audit.commit_tid,
        events=audit.events,
        completion_before=audit.completion_before,
        completion_after=audit.completion_after,
        callback_flags=audit.callback_flags_at_c9c,
        c98_pair_after=audit.c98_pair_after,
        c9c_pair_after=audit.c9c_pair_after,
        anchor_transform=audit.transform,
        anchor_linear=audit.linear_velocity,
        frame0_transform=frames[0].transform,
        frame0_linear=frames[0].linear_velocity,
    )


class NaturalPrerollAnchorTests(unittest.TestCase):
    def test_unbound_round_trip_is_exact_and_fixed_size(self) -> None:
        anchor = make_unbound()
        blob = encode_anchor(anchor)
        self.assertEqual(len(blob), SIZE)
        self.assertEqual(decode_anchor(blob, require_bound=False), anchor)

    def test_binding_is_exclusive_and_verifies_both_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory) / "raw.a9npa1"
            output = Path(directory) / "bound.a9npa1"
            raw.write_bytes(encode_anchor(make_unbound()))
            bound = bind_anchor(raw, REPORT, RECORDING, output)
            self.assertEqual(bound.flags, BOUND_FLAGS)
            verify_source_binding(decode_anchor(output.read_bytes()), REPORT, RECORDING)
            with self.assertRaises(FileExistsError):
                bind_anchor(raw, REPORT, RECORDING, output)

    def test_rejects_tampered_source_hash_and_frame0(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory) / "raw.a9npa1"
            output = Path(directory) / "bound.a9npa1"
            raw.write_bytes(encode_anchor(make_unbound()))
            bind_anchor(raw, REPORT, RECORDING, output)
            bound = decode_anchor(output.read_bytes())
            bad_hash = dataclasses.replace(bound, recording_sha256=bytes([1]) * 32)
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                verify_source_binding(bad_hash, REPORT, RECORDING)
            changed = bytearray(bound.frame0_linear)
            changed[0] ^= 1
            bad_frame = dataclasses.replace(bound, frame0_linear=bytes(changed))
            with self.assertRaisesRegex(ValueError, "frame-0"):
                verify_source_binding(bad_frame, REPORT, RECORDING)

    def test_rejects_invalid_certificate_and_nonfinite_physics(self) -> None:
        anchor = make_unbound()
        bad_events = dataclasses.replace(anchor, events=(1, 2, 3, 4, 5, 6, 6))
        with self.assertRaisesRegex(ValueError, "event certificate"):
            decode_anchor(encode_anchor(bad_events), require_bound=False)
        bad_linear = struct.pack("<3f", float("nan"), 0.0, 0.0)
        nonfinite = dataclasses.replace(anchor, anchor_linear=bad_linear)
        with self.assertRaisesRegex(ValueError, "anchor physics"):
            decode_anchor(encode_anchor(nonfinite), require_bound=False)

    def test_anchor_match_uses_anchor_to_frame0_bounded_tolerance(self) -> None:
        anchor = make_unbound()
        exact = anchor.anchor_transform + anchor.anchor_linear
        self.assertTrue(anchor_matches(exact, anchor))
        values = list(struct.unpack("<19f", exact))
        values[0] += 100.0
        self.assertFalse(anchor_matches(struct.pack("<19f", *values), anchor))


if __name__ == "__main__":
    unittest.main()
