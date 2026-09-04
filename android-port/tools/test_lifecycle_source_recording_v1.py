#!/usr/bin/env python3
"""Offline A9USR5 semantic-envelope regression using a proven A9USR2 pair."""

from __future__ import annotations

import pathlib
import struct
import unittest

from lifecycle_source_recording_v1 import (
    MAGIC,
    RACE_LIFECYCLE_FLAG,
    VERSION,
    verify_lifecycle_source_capture,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT = ROOT / "evidence" / "a9tas_input_cycle_source_360f_20260821_170918_621.a9usr2"
RECORDING = ROOT / "evidence" / "a9tas_input_cycle_source_360f_20260821_170918_621.a9utk1"


def lifecycle_envelope() -> bytearray:
    blob = bytearray(REPORT.read_bytes())
    blob[:8] = MAGIC
    struct.pack_into("<I", blob, 8, VERSION)
    flags = struct.unpack_from("<I", blob, 20)[0]
    struct.pack_into("<I", blob, 20, flags | RACE_LIFECYCLE_FLAG)
    return blob


class LifecycleSourceRecordingTests(unittest.TestCase):
    def test_proven_physical_pair_accepts_lifecycle_envelope(self) -> None:
        result = verify_lifecycle_source_capture(
            bytes(lifecycle_envelope()), RECORDING.read_bytes())
        self.assertEqual(result.captured_frames, 360)

    def test_wrong_magic_rejected(self) -> None:
        blob = lifecycle_envelope()
        blob[0] ^= 1
        with self.assertRaisesRegex(ValueError, "magic/version"):
            verify_lifecycle_source_capture(bytes(blob), RECORDING.read_bytes())

    def test_missing_lifecycle_witness_rejected(self) -> None:
        blob = lifecycle_envelope()
        flags = struct.unpack_from("<I", blob, 20)[0]
        struct.pack_into("<I", blob, 20, flags & ~RACE_LIFECYCLE_FLAG)
        with self.assertRaisesRegex(ValueError, "lifecycle witness"):
            verify_lifecycle_source_capture(bytes(blob), RECORDING.read_bytes())


if __name__ == "__main__":
    unittest.main()

