#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("parse_physics_interval_readonly_v1.py")
SPEC = importlib.util.spec_from_file_location("parse_physics_interval_readonly_v1", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def make_receipt(path: Path, *, alternate: int = 0, changes: int = 0,
                 flags: int | None = None) -> None:
    interval_bits = struct.unpack("<I", struct.pack("<f", 1.0 / 60.0))[0]
    if flags is None:
        flags = (MODULE.CLEAN | MODULE.TARGET_VERIFIED | MODULE.INLINE_ALL |
                 MODULE.IDENTITY_STABLE | MODULE.INTERVAL_STABLE)
    samples = [
        MODULE.SAMPLE.pack(100, 0, 0, 0, 0, 0x3000, 0x4000, interval_bits,
                           0x3C000000, 0x1F, 0),
        MODULE.SAMPLE.pack(200, 0, 0, 0, 0, 0x3000, 0x4000, interval_bits,
                           0x3B800000, 0x1F, 0),
    ]
    header = MODULE.HEADER.pack(
        MODULE.MAGIC, MODULE.VERSION, MODULE.HEADER.size, MODULE.SAMPLE.size,
        flags, 123, 0x1000, 0x2000, 0x3000, 0x4000, 50, 1000, 10,
        len(samples), 0, 0, alternate, changes, interval_bits, 0,
    )
    path.write_bytes(header + b"".join(samples))


def make_car_physics_receipt(path: Path, *, vptr_rva: int = 0x7EED420,
                             getter_rva: int = 0x3695740) -> None:
    interval_bits = struct.unpack("<I", struct.pack("<f", 1.0 / 60.0))[0]
    base = 0x10000000
    object_address = 0x30002A78
    sample_flags = (
        MODULE.SAMPLE_READ_OK | MODULE.SAMPLE_CONTEXT_OK |
        MODULE.SAMPLE_BACKEND_OK | MODULE.SAMPLE_INTERVAL_FINITE |
        MODULE.SAMPLE_OPTIONS_HEAD_OK
    )
    samples = [
        MODULE.SAMPLE.pack(
            timestamp, object_address, base + vptr_rva,
            base + getter_rva, 0x1111222233334444, 0x4000, 0x5000,
            interval_bits, 0x3C000000, sample_flags, 0)
        for timestamp in (100, 200, 300)
    ]
    flags = (MODULE.CLEAN | MODULE.TARGET_VERIFIED |
             MODULE.IDENTITY_STABLE | MODULE.INTERVAL_STABLE)
    header = MODULE.HEADER.pack(
        MODULE.MAGIC, MODULE.VERSION, MODULE.HEADER.size, MODULE.SAMPLE.size,
        flags, 123, base, 0x2000, 0x4000, 0x5000, 50, 1000, 10,
        len(samples), 0, 0, len(samples), 0, interval_bits, 0)
    path.write_bytes(header + b"".join(samples))


class ParserTests(unittest.TestCase):
    def test_valid_inline_stable_receipt_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "valid.a9pio1"
            make_receipt(path)
            result = MODULE.parse(path)
            self.assertTrue(result["passed"])
            self.assertEqual(result["sample_count"], 2)
            self.assertEqual(result["ptrace_calls"], 0)
            self.assertEqual(result["game_writes"], 0)

    def test_alternate_options_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "alternate.a9pio1"
            flags = (MODULE.CLEAN | MODULE.TARGET_VERIFIED |
                     MODULE.IDENTITY_STABLE | MODULE.INTERVAL_STABLE)
            make_receipt(path, alternate=1, flags=flags)
            self.assertFalse(MODULE.parse(path)["passed"])

    def test_interval_change_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "changed.a9pio1"
            flags = (MODULE.CLEAN | MODULE.TARGET_VERIFIED | MODULE.INLINE_ALL |
                     MODULE.IDENTITY_STABLE)
            make_receipt(path, changes=1, flags=flags)
            self.assertFalse(MODULE.parse(path)["passed"])

    def test_exact_car_physics_profile_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "car.a9pio2"
            make_car_physics_receipt(path)
            result = MODULE.parse(path, "car-physics")
            self.assertTrue(result["passed"])
            self.assertEqual(result["profile"], "car-physics")
            self.assertEqual(result["step_options"], ["0x30002a78"])

    def test_car_physics_receipt_fails_inline_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "car.a9pio2"
            make_car_physics_receipt(path)
            self.assertFalse(MODULE.parse(path)["passed"])

    def test_car_physics_uses_runtime_build_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "car.a9pio2"
            profile = root / "candidate.a9profile.bin"
            gameplay_delta = 0x23790
            vptr_rva = 0x7EED420 + gameplay_delta
            getter_rva = 0x3695740 + gameplay_delta
            make_car_physics_receipt(
                receipt, vptr_rva=vptr_rva, getter_rva=getter_rva)
            blob = bytearray(384)
            blob[:8] = b"A9BPR1\0\0"
            struct.pack_into("<II", blob, 8, 1, 384)
            struct.pack_into("<Q", blob, 32, 0x3695474 + gameplay_delta)
            struct.pack_into("<Q", blob, 128, vptr_rva)
            profile.write_bytes(blob)
            self.assertTrue(
                MODULE.parse(receipt, "car-physics", profile)["passed"])
            self.assertFalse(MODULE.parse(receipt, "car-physics")["passed"])

    def test_truncated_receipt_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "short.a9pio1"
            path.write_bytes(b"A9PIO1")
            with self.assertRaisesRegex(ValueError, "truncated"):
                MODULE.parse(path)


if __name__ == "__main__":
    unittest.main()
