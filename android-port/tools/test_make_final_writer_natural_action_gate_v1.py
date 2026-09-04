from __future__ import annotations

import hashlib
import pathlib
import unittest

from make_final_writer_natural_action_gate_v1 import (
    DEFAULT_START_FRAME,
    SEQUENCE,
    SOURCE_SHA256,
    build,
)
from make_final_writer_target_blob_v1 import decode_target_blob
from unified_tick_recording_v1 import SKIP_NITRO, decode_recording


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "evidence" / "a9tas_final_writer_gate_360f_20260821.a9utk1"


class CompositeGateTests(unittest.TestCase):
    def test_reviewed_source_projection(self) -> None:
        source = SOURCE.read_bytes()
        self.assertEqual(hashlib.sha256(source).hexdigest(), SOURCE_SHA256)
        recording, target = build(source)
        interval, frames = decode_recording(recording)
        self.assertEqual((interval, len(frames)), (16667, 360))
        self.assertTrue(all((frame.skip_flags & SKIP_NITRO) == 0
                            for frame in frames))
        observed = tuple(frames[DEFAULT_START_FRAME + offset].nitro_activations
                         for offset in range(len(SEQUENCE)))
        self.assertEqual(observed, SEQUENCE)
        self.assertEqual(sum(frame.nitro_activations for frame in frames), 3)
        decoded = decode_target_blob(target, expected_recording=recording)
        self.assertEqual(decoded["frame_count"], 360)

    def test_rejects_unpinned_source(self) -> None:
        source = bytearray(SOURCE.read_bytes())
        source[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "reviewed"):
            build(bytes(source))


if __name__ == "__main__":
    unittest.main()
