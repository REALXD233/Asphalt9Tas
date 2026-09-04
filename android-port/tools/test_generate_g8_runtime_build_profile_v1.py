#!/usr/bin/env python3
"""Round-trip tests for the channel-agnostic G8 runtime profile compiler."""

from __future__ import annotations

import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

import generate_g8_runtime_build_profile_v1 as generator


ROOT = Path(__file__).resolve().parents[1]
PROFILE_DIR = ROOT / "build" / "generated-profiles"


class RuntimeBuildProfileTests(unittest.TestCase):
    def test_reference_and_candidate_round_trip(self) -> None:
        cases = (
            ("reference.a9profile.json", 0xA5D6E3C, 0x3695474, 0x7F3C1A8),
            (
                "439fd7f94ef570d9.a9profile.json",
                0xA62112C,
                0x36B8C04,
                0x7F74E30,
            ),
        )
        for filename, image_size, interval_rva, main_vtable in cases:
            with self.subTest(filename=filename):
                path = PROFILE_DIR / filename
                source = path.read_bytes()
                blob = generator.load_and_compile(path)
                self.assertGreater(len(blob), generator.SIZE)
                prefix = struct.unpack_from("<8sIIIIQ", blob, 0)
                self.assertEqual(prefix, (
                    generator.MAGIC,
                    generator.VERSION,
                    generator.SIZE,
                    generator.REQUIRED_FLAGS,
                    0,
                    image_size,
                ))
                qwords = struct.unpack_from("<25Q", blob, 32)
                self.assertEqual(qwords[0], interval_rva)
                self.assertEqual(qwords[7], main_vtable)
                self.assertEqual(qwords[16], qwords[10])
                self.assertEqual(blob[232:264], bytes.fromhex(
                    json.loads(source.decode("utf-8"))["candidate"]["sha256"]
                ))
                self.assertEqual(blob[264:296], hashlib.sha256(source).digest())
                self.assertEqual(blob[316:generator.SIZE], bytes(68))
                annex = struct.unpack_from("<8sIIIIII", blob, generator.SIZE)
                self.assertEqual(annex[0], generator.ANNEX_MAGIC)
                self.assertEqual(annex[1], generator.ANNEX_VERSION)
                self.assertEqual(annex[2], generator.ANNEX_HEADER_SIZE)
                self.assertEqual(annex[3], generator.VEHICLE_RVA_COUNT)
                self.assertGreater(annex[4], 0)
                self.assertEqual(annex[5], len(blob))
                self.assertEqual(annex[6], 0)

    def test_fail_closed_metadata(self) -> None:
        path = PROFILE_DIR / "reference.a9profile.json"
        source = path.read_bytes()
        document = json.loads(source.decode("utf-8"))
        for field, value in (
            ("write_authorized", True),
            ("channel_specific_literals", 1),
            ("pending", []),
        ):
            changed = dict(document)
            changed[field] = value
            with self.subTest(field=field):
                with self.assertRaises(generator.ProfileError):
                    generator.compile_profile(changed, source)

    def test_atomic_file_output(self) -> None:
        source = PROFILE_DIR / "reference.a9profile.json"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "nested" / "profile.bin"
            self.assertEqual(generator.main([str(source), str(output)]), 0)
            self.assertEqual(output.read_bytes(), generator.load_and_compile(source))


if __name__ == "__main__":
    unittest.main()
