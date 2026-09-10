#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import struct
import tempfile
import unittest
import uuid
from argparse import Namespace
from pathlib import Path

import a9tas_library_v1 as library
from a9tas_recording_v1 import (
    A9G4R2_FLAGS, A9G4R2_FRAME, A9G4R2_HEADER, A9G4R2_INTERVAL,
    A9G4R2_MAGIC, A9G4R2_SKIP, A9G4R2_VERSION, HEADER_SIZE, RecordingError,
    HEADER, build_manifest, decode_a9g4r2, decode_archive, encode_archive,
)


FIXED_DELTA_US = 16667
NATIVE_SHA = "43" * 32
PROFILE_SHA = "8e" * 32
BUILD_ID = "79" * 20
RECORDING_ID = str(uuid.UUID("12345678-1234-5678-9234-567812345678"))


def make_recording(frames: int = 3) -> bytes:
    intervals = frames
    header = A9G4R2_HEADER.pack(
        A9G4R2_MAGIC, A9G4R2_VERSION, A9G4R2_HEADER.size,
        A9G4R2_FRAME.size, A9G4R2_INTERVAL.size, frames, intervals,
        FIXED_DELTA_US, A9G4R2_FLAGS, 0x12345678, 1, 0, bytes(8),
    )
    body = bytearray()
    identity = struct.pack("<16f", *(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ))
    linear = struct.pack("<3f", 1.0, 0.0, 0.0)
    for tick in range(frames):
        body += A9G4R2_FRAME.pack(
            tick, tick * FIXED_DELTA_US * 1000, 0.25, -1.0, 0.0,
            int(tick == 1), A9G4R2_SKIP, 0, bytes(3),
            0.0, 0.0, 0.0, 0.0, 0.0, identity, linear, 0x7, 0,
        )
    interval_bits = struct.unpack("<I", struct.pack("<f", 1 / 60))[0]
    for tick in range(frames):
        body += A9G4R2_INTERVAL.pack(tick, 0, interval_bits)
    return header + bytes(body)


def make_manifest(recording: bytes, target_tick: int = 2) -> dict:
    return build_manifest(
        source=decode_a9g4r2(recording), recording_id=RECORDING_ID,
        title="浦东 F5 测试", created_utc="2026-08-26T12:00:00Z",
        game_package="com.aligames.kuang.kybc.huawei",
        game_version="6.0.0k", native_sha256=NATIVE_SHA,
        build_id=BUILD_ID, build_profile_sha256=PROFILE_SHA,
        map_name="浦东崛起", car_name="F5", control_mode="manual",
        target_tick=target_tick,
    )


class RecordingContainerTests(unittest.TestCase):
    def test_sparse_phase_archive_roundtrip(self) -> None:
        raw = bytearray(make_recording())
        raw = raw[:64 + 3 * 144]  # Three logical ticks with no native steps.
        struct.pack_into('<I', raw, 28, 0)
        struct.pack_into('<I', raw, 32, 6944)
        for tick in range(3):
            struct.pack_into('<Q', raw, 64 + tick * 144 + 8, tick * 6944000)
        for version, flags in ((4, 0x3f), (5, 0x7f)):
            struct.pack_into('<I', raw, 8, version)
            struct.pack_into('<I', raw, 36, flags)
            raw[56:64] = bytes(8) if version == 4 else struct.pack('<ff', -0.009, 1 / 60)
            source = bytes(raw)
            packed = encode_archive(make_manifest(source), source)
            self.assertEqual(decode_archive(packed).recording, source)
            self.assertEqual(decode_a9g4r2(source).interval_count, 0)
        for residual in (float('nan'), float('inf'), 0.01, -1.0):
            struct.pack_into('<f', raw, 56, residual)
            with self.assertRaises(RecordingError):
                decode_a9g4r2(bytes(raw))

    def test_round_trip_is_byte_exact_and_deterministic(self) -> None:
        recording = make_recording()
        manifest = make_manifest(recording)
        first = encode_archive(manifest, recording)
        second = encode_archive(manifest, recording)
        self.assertEqual(first, second)
        archive = decode_archive(first)
        self.assertEqual(archive.recording, recording)
        self.assertEqual(archive.recording_sha256,
                         hashlib.sha256(recording).hexdigest())
        self.assertEqual(archive.manifest["recording"]["target_tick"], 2)

    def test_hash_size_tick_and_canonical_manifest_fail_closed(self) -> None:
        blob = bytearray(encode_archive(make_manifest(make_recording()),
                                        make_recording()))
        for offset, expected in ((64, "archive.manifest_hash"),
                                 (HEADER_SIZE + 1, "archive.manifest_hash"),
                                 (len(blob) - 1, "archive.recording_hash")):
            damaged = bytearray(blob)
            damaged[offset] ^= 1
            with self.assertRaisesRegex(RecordingError, expected):
                decode_archive(bytes(damaged))
        with self.assertRaisesRegex(RecordingError, "target_tick"):
            encode_archive(make_manifest(make_recording(), 3), make_recording())

        valid = encode_archive(make_manifest(make_recording()), make_recording())
        fields = list(HEADER.unpack_from(valid))
        manifest_size = fields[4]
        manifest = json.loads(valid[HEADER_SIZE:HEADER_SIZE + manifest_size])
        noncanonical = json.dumps(manifest, ensure_ascii=False, indent=1).encode("utf-8")
        fields[4] = len(noncanonical)
        fields[13] = hashlib.sha256(noncanonical).digest()
        forged = HEADER.pack(*fields) + noncanonical + valid[HEADER_SIZE + manifest_size:]
        with self.assertRaisesRegex(RecordingError, "manifest_not_canonical"):
            decode_archive(forged)

    def test_source_tick_corruption_is_rejected(self) -> None:
        recording = bytearray(make_recording())
        struct.pack_into("<Q", recording, A9G4R2_HEADER.size, 9)
        with self.assertRaisesRegex(RecordingError, "a9g4r2.frame.0"):
            decode_a9g4r2(bytes(recording))

    def test_library_rename_target_list_unpack_delete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "source.a9g4r2"
            archive_path = root / "library" / "sample.a9tas"
            raw.write_bytes(make_recording())
            result = library.command_pack(Namespace(
                recording=raw, output=archive_path, title="First",
                recording_id=RECORDING_ID, created_utc="2026-08-26T12:00:00Z",
                game_package="com.aligames.kuang.kybc.huawei",
                game_version="6.0.0k", native_sha256=NATIVE_SHA,
                build_id=BUILD_ID, profile_sha256=PROFILE_SHA,
                map_name="浦东崛起", car_name="F5", control_mode="manual",
                target_tick="end", notes=""))
            self.assertEqual(result["target_tick"], 2)
            before = read_bytes = decode_archive(archive_path.read_bytes()).recording
            renamed = library.command_rename(Namespace(archive=archive_path,
                                                       title="Renamed"))
            self.assertEqual(renamed["title"], "Renamed")
            targeted = library.command_set_target(Namespace(
                archive=archive_path, target_tick="1"))
            self.assertEqual(targeted["target_tick"], 1)
            self.assertEqual(decode_archive(archive_path.read_bytes()).recording,
                             before)
            listing = library.command_list(Namespace(library=archive_path.parent))
            self.assertEqual(len(listing["recordings"]), 1)
            self.assertEqual(listing["invalid"], [])
            bad = archive_path.parent / "bad.a9tas"
            bad.write_bytes(b"bad")
            listing = library.command_list(Namespace(library=archive_path.parent))
            self.assertEqual(len(listing["recordings"]), 1)
            self.assertEqual(len(listing["invalid"]), 1)
            unpacked = root / "unpacked.a9g4r2"
            library.command_unpack(Namespace(archive=archive_path,
                                             output=unpacked))
            self.assertEqual(unpacked.read_bytes(), read_bytes)
            with self.assertRaisesRegex(RecordingError, "requires_yes"):
                library.command_delete(Namespace(library=archive_path.parent,
                                                 archive=archive_path, yes=False))
            library.command_delete(Namespace(library=archive_path.parent,
                                             archive=archive_path, yes=True))
            self.assertFalse(archive_path.exists())


if __name__ == "__main__":
    unittest.main()
