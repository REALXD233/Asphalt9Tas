from __future__ import annotations

import hashlib
import unittest

from make_final_writer_prefix_v1 import make_prefix
from make_final_writer_target_blob_v1 import decode_target_blob
from unified_tick_recording_v1 import (
    REQUIRED_FRAME_FLAGS,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)


class FinalWriterPrefixTest(unittest.TestCase):
    @staticmethod
    def source(count: int = 900) -> bytes:
        frames = []
        transform = bytes.fromhex("0000803f") * 16
        linear = bytes.fromhex("00000000") * 3
        for index in range(count):
            frames.append(UnifiedTickFrameV1(
                tick=1000 + index,
                monotonic_ns=10_000_000 + index * 16_667_000,
                steering=0.0, brake=-1.0, accelerator=0.0,
                nitro_activations=0, skip_flags=0x7C, respawn=False,
                barrel_angular=(0.0, 0.0, 0.0),
                barrel_rbx=(0.0, 0.0), transform=transform,
                linear_velocity=linear, flags=REQUIRED_FRAME_FLAGS))
        return encode_recording(frames, fixed_interval_us=16667)

    def test_gate_prefixes_are_reencoded_and_hash_bound(self) -> None:
        source = self.source()
        for count in (30, 360, 900):
            recording, target = make_prefix(source, count)
            interval, frames = decode_recording(recording)
            decoded = decode_target_blob(target, expected_recording=recording)
            self.assertEqual((interval, len(frames)), (16667, count))
            self.assertEqual(decoded["frame_count"], count)
            self.assertEqual(decoded["source_sha256"],
                             hashlib.sha256(recording).digest())

    def test_unsupported_or_oversized_prefix_fails(self) -> None:
        source = self.source(360)
        with self.assertRaises(ValueError):
            make_prefix(source, 31)
        with self.assertRaises(ValueError):
            make_prefix(source, 900)


if __name__ == "__main__":
    unittest.main()
