#!/usr/bin/env python3
"""Cross-binding regression tests for aligned synchronized replay."""

from __future__ import annotations

import struct
import unittest

from aligned_tick_replay_v1 import verify_aligned_replay
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    COMMITTED,
    CORRECTION_CORRECTED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    REQUIRED_HEADER_FLAGS,
    STEERING_APPLIED,
    _HEADER,
)
from parse_unified_executor_report_v5 import FRAME_SIZE, HEADER_SIZE, MAGIC, VERSION, _FRAME
from test_synchronized_tick_recording_v1 import make_capture
from unified_tick_recording_v1 import SUPPORTED_BUILD_ID, decode_recording


def make_replay_report(
    recording_blob: bytes, *, first_far: bool = False, target_mismatch: bool = False
) -> bytes:
    fixed_interval, frames = decode_recording(recording_blob)
    audits: list[bytes] = []
    flags = (
        STEERING_APPLIED
        | CORRECTION_CORRECTED
        | AUDIT_EXACT
        | GATE2_COMPLETE
        | COMMITTED
        | PREFIX_CERTIFIED
    )
    for index, frame in enumerate(frames):
        transform = frame.transform
        linear = frame.linear_velocity
        target_transform = transform
        if target_mismatch and index == 0:
            target_transform = struct.pack("<f", 123.0) + transform[4:]
        target_values = struct.unpack("<19f", transform + linear)
        divergence = 100.0 if first_far and index == 0 else 0.02
        before_values = tuple(value + divergence for value in target_values)
        before_payload = struct.pack("<19f", *before_values)
        before = bytearray(804)
        before[0x10:0x50] = before_payload[:64]
        before[0x150:0x15C] = before_payload[64:]
        immediate = bytearray(before)
        immediate[0x10:0x50] = target_transform
        immediate[0x150:0x15C] = linear
        steering_bits = struct.unpack("<I", struct.pack("<f", frame.steering))[0]
        first_event = index * 7 + 1
        audits.append(
            _FRAME.pack(
                frame.tick,
                frame.monotonic_ns,
                2767,
                flags,
                16600,
                fixed_interval,
                *range(first_event, first_event + 7),
                100 + index,
                101 + index,
                1,
                12624,
                bytes(2),
                steering_bits,
                bytes(4),
                steering_bits << 32 | 0x1234,
                steering_bits << 32 | 0x1234,
                target_transform,
                linear,
                bytes(before),
                bytes(immediate),
            )
        )
    count = len(frames)
    qwords = (
        2668,
        0x100000,
        0x200000,
        0x300000,
        0x400000,
        0x400120,
        0x400130,
        0x500064,
        0x600000,
        0x600028,
        0x500000,
        0x700000,
        0x800000,
        0x800010,
        0x800150,
        count * 7,
        count,
        count * 2,
        0,
        count,
        0,
        0,
        0,
        0,
        count,
        0,
        0,
        0,
        0,
    )
    assert len(qwords) == 29
    header = _HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FRAME_SIZE,
        REQUIRED_HEADER_FLAGS,
        count,
        count,
        SUPPORTED_BUILD_ID,
        0,
        *qwords,
        268,
        268,
    )
    return header + b"".join(audits)


class AlignedTickReplayTests(unittest.TestCase):
    def test_accepts_cross_bound_aligned_replay(self) -> None:
        source_report, recording = make_capture(2)
        result = verify_aligned_replay(
            make_replay_report(recording), source_report, recording
        )
        self.assertEqual((result.frames, result.equal_frames, result.corrected_frames), (2, 0, 2))
        self.assertLessEqual(result.maximum_first_frame_ratio, 1.0)

    def test_rejects_first_frame_beyond_guard(self) -> None:
        source_report, recording = make_capture(2)
        with self.assertRaisesRegex(ValueError, "alignment guard"):
            verify_aligned_replay(
                make_replay_report(recording, first_far=True),
                source_report,
                recording,
            )

    def test_rejects_replay_target_not_bound_to_recording(self) -> None:
        source_report, recording = make_capture(2)
        with self.assertRaisesRegex(ValueError, "final payload target"):
            verify_aligned_replay(
                make_replay_report(recording, target_mismatch=True),
                source_report,
                recording,
            )


if __name__ == "__main__":
    unittest.main()
