#!/usr/bin/env python3
"""Synthetic cross-binding tests for natural steering+brake replay."""

from __future__ import annotations

import pathlib
import struct
import unittest

from aligned_brake_replay_v1 import verify_aligned_brake_replay
from parse_unified_executor_report_v2 import _HEADER as REPLAY_HEADER
from parse_unified_executor_report_v5 import (
    FRAME_SIZE as REPLAY_FRAME_SIZE,
    HEADER_SIZE as REPLAY_HEADER_SIZE,
    _FRAME as REPLAY_FRAME,
)
from parse_unified_executor_report_v6 import BRAKE_APPLIED_NATURAL
from synchronized_brake_recording_v1 import MAGIC, REQUIRED_FLAGS, VERSION
from synchronized_tick_recording_v1 import (
    FRAME_AUDIT_SIZE,
    HEADER_SIZE as SOURCE_HEADER_SIZE,
    _FRAME as SOURCE_FRAME,
    _HEADER as SOURCE_HEADER,
)
from unified_tick_recording_v1 import FRAME_SIZE, HEADER_SIZE


ROOT = pathlib.Path(__file__).resolve().parents[1]
INPUT = ROOT / "evidence" / "a9tas_natural_preroll_source_run1_20260817.a9utk1"
SOURCE = ROOT / "evidence" / "a9tas_natural_preroll_source_run1_20260817.a9usr1"
REPLAY = ROOT / "evidence" / "a9tas_natural_preroll_full_replay_run1_20260817.a9uer5"


def make_combined(brake_bits: int = 0xBF800000) -> tuple[bytes, bytes, bytes]:
    input_blob = bytearray(INPUT.read_bytes())
    frame_count = (len(input_blob) - HEADER_SIZE) // FRAME_SIZE
    for index in range(frame_count):
        offset = HEADER_SIZE + index * FRAME_SIZE
        struct.pack_into("<I", input_blob, offset + 20, brake_bits)
        skip = struct.unpack_from("<I", input_blob, offset + 32)[0]
        struct.pack_into("<I", input_blob, offset + 32, skip & ~(1 << 1))

    source_blob = SOURCE.read_bytes()
    source_header = list(SOURCE_HEADER.unpack_from(source_blob))
    source_header[0], source_header[1], source_header[4] = MAGIC, VERSION, REQUIRED_FLAGS
    source_frames: list[bytes] = []
    for index in range(frame_count):
        frame = list(SOURCE_FRAME.unpack_from(
            source_blob, SOURCE_HEADER_SIZE + index * FRAME_AUDIT_SIZE
        ))
        frame[17] = (frame[17] & 0xFFFFFFFF00000000) | brake_bits
        frame[18] = (frame[18] & 0xFFFFFFFF00000000) | brake_bits
        source_frames.append(SOURCE_FRAME.pack(*frame))

    replay_blob = REPLAY.read_bytes()
    replay_header = list(REPLAY_HEADER.unpack_from(replay_blob))
    replay_header[0], replay_header[1] = b"A9UER6\0\0", 6
    replay_frames: list[bytes] = []
    for index in range(frame_count):
        frame = list(REPLAY_FRAME.unpack_from(
            replay_blob, REPLAY_HEADER_SIZE + index * REPLAY_FRAME_SIZE
        ))
        frame[3] |= BRAKE_APPLIED_NATURAL
        frame[20] = (frame[20] & 0xFFFFFFFF00000000) | brake_bits
        frame[21] = (frame[21] & 0xFFFFFFFF00000000) | brake_bits
        replay_frames.append(REPLAY_FRAME.pack(*frame))
    return (
        REPLAY_HEADER.pack(*replay_header) + b"".join(replay_frames),
        SOURCE_HEADER.pack(*source_header) + b"".join(source_frames),
        bytes(input_blob),
    )


class AlignedBrakeReplayTests(unittest.TestCase):
    def test_accepts_combined_pair_replay(self) -> None:
        replay, source, recording = make_combined()
        result = verify_aligned_brake_replay(replay, source, recording)
        self.assertEqual(result.frames, 5)
        self.assertLessEqual(result.maximum_first_frame_ratio, 1.0)

    def test_rejects_one_boundary_brake_tamper(self) -> None:
        replay, source, recording = make_combined()
        changed = bytearray(replay)
        frame = list(REPLAY_FRAME.unpack_from(changed, REPLAY_HEADER_SIZE))
        frame[21] ^= 1
        changed[REPLAY_HEADER_SIZE : REPLAY_HEADER_SIZE + REPLAY_FRAME_SIZE] = REPLAY_FRAME.pack(*frame)
        with self.assertRaisesRegex(ValueError, "brake pair writes differ"):
            verify_aligned_brake_replay(bytes(changed), source, recording)


if __name__ == "__main__":
    unittest.main()
