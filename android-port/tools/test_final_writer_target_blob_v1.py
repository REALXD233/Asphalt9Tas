#!/usr/bin/env python3
"""Offline unit tests for strict A9UTK1 -> A9FWT1 projection."""

from __future__ import annotations

import pathlib
import struct
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from make_final_writer_target_blob_v1 import (  # noqa: E402
    HEADER_SIZE,
    RECORD_SIZE,
    decode_target_blob,
    encode_target_blob,
)
from unified_tick_recording_v1 import (  # noqa: E402
    SKIP_TRANSFORM,
    UnifiedTickFrameV1,
    encode_recording,
)


def frame(index: int, *, skip_flags: int = 0) -> UnifiedTickFrameV1:
    return UnifiedTickFrameV1(
        tick=100 + index,
        monotonic_ns=1_000_000 + index * 16_667_000,
        steering=0.0,
        brake=0.0,
        accelerator=1.0,
        nitro_activations=0,
        skip_flags=skip_flags,
        respawn=False,
        barrel_angular=(0.0, 0.0, 0.0),
        barrel_rbx=(0.0, 0.0),
        transform=struct.pack("<16f", *[float(index + x) for x in range(16)]),
        linear_velocity=struct.pack("<3f", float(index), 2.0, 3.0),
    )


class TargetBlobTests(unittest.TestCase):
    def test_round_trip_and_exact_projection(self) -> None:
        source_frames = (frame(0), frame(1), frame(2))
        recording = encode_recording(source_frames, fixed_interval_us=16667)
        blob = encode_target_blob(recording)
        decoded = decode_target_blob(blob, expected_recording=recording)
        self.assertEqual(len(blob), HEADER_SIZE + 3 * RECORD_SIZE)
        self.assertEqual(decoded["frame_count"], 3)
        self.assertEqual(
            decoded["targets"],
            tuple((item.transform, item.linear_velocity) for item in source_frames),
        )

    def test_source_identity_is_mandatory(self) -> None:
        recording = encode_recording((frame(0), frame(1)), fixed_interval_us=16667)
        blob = encode_target_blob(recording)
        changed = bytearray(recording)
        changed[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "source SHA-256 mismatch"):
            decode_target_blob(blob, expected_recording=bytes(changed))

    def test_rejects_wrong_interval(self) -> None:
        recording = encode_recording((frame(0), frame(1)), fixed_interval_us=16666)
        with self.assertRaisesRegex(ValueError, "exactly 16667"):
            encode_target_blob(recording)

    def test_rejects_transform_skip(self) -> None:
        recording = encode_recording(
            (frame(0), frame(1, skip_flags=SKIP_TRANSFORM)),
            fixed_interval_us=16667,
        )
        with self.assertRaisesRegex(ValueError, "transform correction is disabled"):
            encode_target_blob(recording)

    def test_rejects_single_frame(self) -> None:
        recording = encode_recording((frame(0),), fixed_interval_us=16667)
        with self.assertRaisesRegex(ValueError, "2..3600"):
            encode_target_blob(recording)

    def test_rejects_noncanonical_target_reserved(self) -> None:
        recording = encode_recording((frame(0), frame(1)), fixed_interval_us=16667)
        blob = bytearray(encode_target_blob(recording))
        blob[HEADER_SIZE + RECORD_SIZE - 1] = 1
        with self.assertRaisesRegex(ValueError, "reserved field"):
            decode_target_blob(bytes(blob), expected_recording=recording)


if __name__ == "__main__":
    unittest.main()
