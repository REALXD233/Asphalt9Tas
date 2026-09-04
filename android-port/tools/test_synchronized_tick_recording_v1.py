#!/usr/bin/env python3
"""Regression tests for synchronized A9USR1/A9UTK1 capture verification."""

from __future__ import annotations

import struct
import unittest

from synchronized_tick_recording_v1 import (
    FRAME_AUDIT_SIZE,
    HEADER_SIZE,
    MAGIC,
    RECORDED_SKIP_FLAGS,
    REQUIRED_FLAGS,
    VERSION,
    _FRAME,
    _HEADER,
    decode_sync_report,
    verify_synchronized_capture,
)
from unified_tick_recording_v1 import (
    SUPPORTED_BUILD_ID,
    UnifiedTickFrameV1,
    encode_recording,
)


def physics(seed: float) -> tuple[bytes, bytes]:
    return (
        struct.pack("<16f", *(seed + value for value in range(16))),
        struct.pack("<3f", seed + 16, seed + 17, seed + 18),
    )


def make_capture(count: int = 2) -> tuple[bytes, bytes]:
    audits: list[bytes] = []
    recording_frames: list[UnifiedTickFrameV1] = []
    for index in range(count):
        transform, linear = physics(float(index))
        steering = 0.25 + index * 0.1
        steering_bits = struct.unpack("<I", struct.pack("<f", steering))[0]
        first_event = index * 7 + 1
        audits.append(
            _FRAME.pack(
                index,
                1_000_000 + index,
                2767,
                12624,
                16000 + index,
                16667,
                *range(first_event, first_event + 7),
                100 + index,
                101 + index,
                1,
                bytes(6),
                0x12345678,
                steering_bits << 32 | 0x12345678,
                transform,
                linear,
            )
        )
        recording_frames.append(
            UnifiedTickFrameV1(
                tick=index,
                monotonic_ns=1_000_000 + index,
                steering=steering,
                brake=0.0,
                accelerator=0.0,
                nitro_activations=0,
                skip_flags=RECORDED_SKIP_FLAGS,
                respawn=False,
                barrel_angular=(0.0, 0.0, 0.0),
                barrel_rbx=(0.0, 0.0),
                transform=transform,
                linear_velocity=linear,
            )
        )
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
        0,
        0,
        0,
        0,
        0,
    )
    report = _HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FRAME_AUDIT_SIZE,
        REQUIRED_FLAGS,
        count,
        count,
        SUPPORTED_BUILD_ID,
        0,
        *qwords,
        268,
        268,
    ) + b"".join(audits)
    recording = encode_recording(recording_frames, fixed_interval_us=16667)
    return report, recording


def mutate_header(report: bytes, index: int, value: int) -> bytes:
    header = list(_HEADER.unpack_from(report))
    header[index] = value
    return _HEADER.pack(*header) + report[HEADER_SIZE:]


class SynchronizedTickRecordingTests(unittest.TestCase):
    def test_accepts_strict_cross_bound_capture(self) -> None:
        report, recording = make_capture()
        decoded = verify_synchronized_capture(report, recording)
        self.assertEqual(decoded.captured_frames, 2)
        self.assertEqual(decoded.frames[1].events, tuple(range(8, 15)))

    def test_rejects_report_error_counter(self) -> None:
        report, _ = make_capture()
        # Header index 27 is read_errors (after the fixed fields and 17 Qs).
        with self.assertRaisesRegex(ValueError, "runtime error"):
            decode_sync_report(mutate_header(report, 27, 1))

    def test_rejects_event_reorder(self) -> None:
        report, _ = make_capture(1)
        fields = list(_FRAME.unpack_from(report, HEADER_SIZE))
        fields[7] = fields[6]
        broken = report[:HEADER_SIZE] + _FRAME.pack(*fields)
        with self.assertRaisesRegex(ValueError, "event order"):
            decode_sync_report(broken)

    def test_rejects_steering_cross_bind_mismatch(self) -> None:
        report, recording = make_capture(1)
        fields = list(_FRAME.unpack_from(report, HEADER_SIZE))
        fields[18] ^= 1 << 32
        broken = report[:HEADER_SIZE] + _FRAME.pack(*fields)
        with self.assertRaisesRegex(ValueError, "steering cross-bind"):
            verify_synchronized_capture(broken, recording)

    def test_rejects_physics_cross_bind_mismatch(self) -> None:
        report, recording = make_capture(1)
        fields = list(_FRAME.unpack_from(report, HEADER_SIZE))
        fields[19] = struct.pack("<f", 999.0) + fields[19][4:]
        broken = report[:HEADER_SIZE] + _FRAME.pack(*fields)
        with self.assertRaisesRegex(ValueError, "physics cross-bind"):
            verify_synchronized_capture(broken, recording)

    def test_rejects_non_dormant_unproven_field(self) -> None:
        report, _ = make_capture(1)
        transform, linear = physics(0.0)
        recording = encode_recording(
            [
                UnifiedTickFrameV1(
                    0,
                    1_000_000,
                    0.25,
                    1.0,
                    0.0,
                    0,
                    RECORDED_SKIP_FLAGS,
                    False,
                    (0.0, 0.0, 0.0),
                    (0.0, 0.0),
                    transform,
                    linear,
                )
            ],
            fixed_interval_us=16667,
        )
        with self.assertRaisesRegex(ValueError, "not dormant"):
            verify_synchronized_capture(report, recording)

    def test_rejects_scope_flag_drift(self) -> None:
        report, _ = make_capture(1)
        transform, linear = physics(0.0)
        recording = encode_recording(
            [
                UnifiedTickFrameV1(
                    0,
                    1_000_000,
                    0.25,
                    0.0,
                    0.0,
                    0,
                    RECORDED_SKIP_FLAGS | 1,
                    False,
                    (0.0, 0.0, 0.0),
                    (0.0, 0.0),
                    transform,
                    linear,
                )
            ],
            fixed_interval_us=16667,
        )
        with self.assertRaisesRegex(ValueError, "scope flags"):
            verify_synchronized_capture(report, recording)


if __name__ == "__main__":
    unittest.main()
