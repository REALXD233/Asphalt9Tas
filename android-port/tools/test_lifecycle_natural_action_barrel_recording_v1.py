#!/usr/bin/env python3
"""Offline tests for the natural-action barrel recording validator."""

from __future__ import annotations

import unittest
import struct
from unittest import mock

from lifecycle_natural_action_barrel_recording_v1 import (
    AUDIT_CERTIFICATE_OFFSET,
    BARREL_CAPTURE_SKIP_FLAGS,
    COMPLETION_WRITE_CERTIFICATE_FLAG,
    verify_lifecycle_natural_action_barrel_capture,
)
from lifecycle_natural_action_recording_v1 import (
    AUDIT_SIZE,
    HEADER_SIZE,
    NATURAL_SKIP_FLAGS,
    REQUIRED_FLAGS,
    NaturalActionCaptureV1,
)
from unified_tick_recording_v1 import (
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)


def _frame(tick: int, angular: tuple[float, float, float], rbx: tuple[float, float], *,
           skip_flags: int = BARREL_CAPTURE_SKIP_FLAGS) -> UnifiedTickFrameV1:
    return UnifiedTickFrameV1(
        tick=tick,
        monotonic_ns=1_000_000 + tick,
        steering=0.0,
        brake=0.0,
        accelerator=0.0,
        nitro_activations=0,
        skip_flags=skip_flags,
        respawn=False,
        barrel_angular=angular,
        barrel_rbx=rbx,
        transform=bytes(64),
        linear_velocity=bytes(12),
    )


def _certified_report(frames: int) -> bytes:
    report = bytearray(HEADER_SIZE + frames * AUDIT_SIZE)
    struct.pack_into("<I", report, 20,
                     REQUIRED_FLAGS | COMPLETION_WRITE_CERTIFICATE_FLAG)
    struct.pack_into("<I", report, 28, frames)
    for index in range(frames):
        report[HEADER_SIZE + index * AUDIT_SIZE + AUDIT_CERTIFICATE_OFFSET] = 1
    return bytes(report)


class LifecycleNaturalActionBarrelRecordingTests(unittest.TestCase):
    def test_masks_only_barrel_fields_before_mature_validation(self) -> None:
        recording = encode_recording(
            (
                _frame(0, (1.0, 2.0, 3.0), (4.0, 5.0)),
                _frame(1, (1.5, 2.5, 3.5), (4.5, 5.5)),
            ),
            fixed_interval_us=16667,
        )
        legacy_result = NaturalActionCaptureV1(2, 1, 1, 0, 0)
        with mock.patch(
            "lifecycle_natural_action_barrel_recording_v1."
            "verify_lifecycle_natural_action_capture",
            return_value=legacy_result,
        ) as verifier:
            result = verify_lifecycle_natural_action_barrel_capture(
                _certified_report(2), recording
            )
        self.assertEqual(result.nonzero_frames, 2)
        self.assertEqual(result.changed_frames, 1)
        synthetic = verifier.call_args.args[1]
        synthetic_report = verifier.call_args.args[0]
        self.assertEqual(struct.unpack_from("<I", synthetic_report, 20)[0],
                         REQUIRED_FLAGS)
        for index in range(2):
            audit = HEADER_SIZE + index * AUDIT_SIZE
            reserved = audit + AUDIT_CERTIFICATE_OFFSET
            self.assertEqual(synthetic_report[reserved:reserved + 6], bytes(6))
            self.assertNotEqual(
                struct.unpack_from("<Q", synthetic_report, audit + 96)[0],
                struct.unpack_from("<Q", synthetic_report, audit + 104)[0],
            )
        _, frames = decode_recording(synthetic)
        self.assertTrue(all(frame.skip_flags == NATURAL_SKIP_FLAGS for frame in frames))
        self.assertTrue(all(frame.barrel_angular == (0.0, 0.0, 0.0) for frame in frames))
        self.assertTrue(all(frame.barrel_rbx == (0.0, 0.0) for frame in frames))

    def test_rejects_old_skip_scope(self) -> None:
        recording = encode_recording(
            (_frame(0, (1.0, 0.0, 0.0), (0.0, 0.0),
                    skip_flags=NATURAL_SKIP_FLAGS),),
            fixed_interval_us=16667,
        )
        with self.assertRaisesRegex(ValueError, "scope flags"):
            verify_lifecycle_natural_action_barrel_capture(b"report", recording)

    def test_rejects_dormant_or_unchanged_source(self) -> None:
        dormant = encode_recording(
            (_frame(0, (-0.0, 0.0, -0.0), (0.0, -0.0)),),
            fixed_interval_us=16667,
        )
        with self.assertRaisesRegex(ValueError, "dormant"):
            verify_lifecycle_natural_action_barrel_capture(b"report", dormant)
        unchanged = encode_recording(
            (
                _frame(0, (1.0, 0.0, 0.0), (0.0, 0.0)),
                _frame(1, (1.0, 0.0, 0.0), (0.0, 0.0)),
            ),
            fixed_interval_us=16667,
        )
        with self.assertRaisesRegex(ValueError, "never change"):
            verify_lifecycle_natural_action_barrel_capture(b"report", unchanged)

    def test_rejects_missing_per_frame_completion_certificate(self) -> None:
        recording = encode_recording(
            (
                _frame(0, (1.0, 0.0, 0.0), (0.0, 0.0)),
                _frame(1, (2.0, 0.0, 0.0), (0.0, 0.0)),
            ),
            fixed_interval_us=16667,
        )
        report = bytearray(_certified_report(2))
        report[HEADER_SIZE + AUDIT_CERTIFICATE_OFFSET] = 0
        with self.assertRaisesRegex(ValueError, "certificate missing"):
            verify_lifecycle_natural_action_barrel_capture(bytes(report), recording)


if __name__ == "__main__":
    unittest.main()
