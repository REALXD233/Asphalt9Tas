#!/usr/bin/env python3
"""A9USR4 bounded action-until-release tests."""

from __future__ import annotations

import hashlib
import pathlib
import struct
import unittest
from dataclasses import replace

from aligned_brake_replay_v1 import verify_aligned_brake_replay
from natural_preroll_anchor_v1 import (
    decode_anchor,
    encode_anchor,
    verify_source_binding,
)
from synchronized_action_until_release_recording_v1 import (
    MAGIC,
    REQUIRED_FLAGS,
    VERSION,
    verify_action_until_release_capture,
)
from synchronized_tick_recording_v1 import _HEADER
from parse_unified_executor_report_v2 import _HEADER as REPLAY_HEADER
from parse_unified_executor_report_v5 import (
    FRAME_SIZE as REPLAY_FRAME_SIZE,
    HEADER_SIZE as REPLAY_HEADER_SIZE,
    _FRAME as REPLAY_FRAME,
)
from parse_unified_executor_report_v6 import BRAKE_APPLIED_NATURAL
from test_synchronized_action_window_recording_v1 import REPLAY, make_action_window
from verify_action_window_anchor_v1 import verify as verify_anchor


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)
ANCHOR = ROOT / "evidence" / "a9tas_natural_preroll_source_run1.a9npa1"


def make_action_until_release() -> tuple[bytes, bytes]:
    report, recording = make_action_window(60)
    header = list(_HEADER.unpack_from(report))
    header[0], header[1], header[4] = MAGIC, VERSION, REQUIRED_FLAGS
    header[5] = 3600
    return _HEADER.pack(*header) + report[_HEADER.size :], recording


class SynchronizedActionUntilReleaseTests(unittest.TestCase):
    def test_source_has_bounded_release_completion(self) -> None:
        self.assertIn("A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1", SOURCE)
        self.assertIn("kActionPostReleaseFrames = 30", SOURCE)
        self.assertIn("kSyncActionReleaseCompleted", SOURCE)
        self.assertIn("report.captured_frames <= report.target_frames", SOURCE)
        self.assertIn("SYNC_ACTION_UNTIL_RELEASE_DIAGNOSTIC", SOURCE)
        self.assertIn("warmup_rejected_candidates", SOURCE)
        self.assertIn("discarded_warmup_candidate", SOURCE)

    def test_accepts_variable_length_capture_and_binds_anchor(self) -> None:
        report, recording = make_action_until_release()
        result = verify_action_until_release_capture(report, recording)
        self.assertEqual(result.maximum_frames, 3600)
        self.assertEqual(result.report.captured_frames, 60)
        self.assertEqual(result.release_index, 30)

        anchor = decode_anchor(ANCHOR.read_bytes())
        bound = replace(
            anchor,
            frame_count=60,
            recording_sha256=hashlib.sha256(recording).digest(),
            report_sha256=hashlib.sha256(report).digest(),
        )
        anchor_blob = encode_anchor(bound)
        verify_source_binding(bound, _write_fixture("report", report), _write_fixture("recording", recording))
        verify_anchor(report, recording, anchor_blob)

    def test_rejects_invalid_maximum_and_postroll(self) -> None:
        report, recording = make_action_until_release()
        changed = bytearray(report)
        struct.pack_into("<I", changed, 24, 59)
        with self.assertRaisesRegex(ValueError, "bounded maximum"):
            verify_action_until_release_capture(bytes(changed), recording)

        # Re-label a valid 180-frame window. Its first release has 90 frames
        # remaining, so it must not masquerade as an auto-stopped A9USR4.
        long_report, long_recording = make_action_window(180)
        header = list(_HEADER.unpack_from(long_report))
        header[0], header[1], header[4] = MAGIC, VERSION, REQUIRED_FLAGS
        header[5] = 3600
        changed_long = _HEADER.pack(*header) + long_report[_HEADER.size :]
        with self.assertRaisesRegex(ValueError, "exactly 30"):
            verify_action_until_release_capture(changed_long, long_recording)

    def test_combined_replay_dispatch_accepts_a9usr4_before_count_guard(self) -> None:
        source, recording = make_action_until_release()
        replay_blob = REPLAY.read_bytes()
        header = list(REPLAY_HEADER.unpack_from(replay_blob))
        header[0], header[1] = b"A9UER6\0\0", 6
        replay_frames: list[bytes] = []
        pattern = ((0.0, 0.0), (0.5, -1.0), (0.5, -1.0), (0.5, -1.0), (0.0, 0.0))
        for index, (steering, brake) in enumerate(pattern):
            steering_bits = struct.unpack("<I", struct.pack("<f", steering))[0]
            brake_bits = struct.unpack("<I", struct.pack("<f", brake))[0]
            frame = list(
                REPLAY_FRAME.unpack_from(
                    replay_blob, REPLAY_HEADER_SIZE + index * REPLAY_FRAME_SIZE
                )
            )
            frame[3] |= BRAKE_APPLIED_NATURAL
            frame[18] = steering_bits
            frame[20] = (steering_bits << 32) | brake_bits
            frame[21] = (steering_bits << 32) | brake_bits
            replay_frames.append(REPLAY_FRAME.pack(*frame))
        replay = REPLAY_HEADER.pack(*header) + b"".join(replay_frames)
        with self.assertRaisesRegex(ValueError, "frame-count mismatch"):
            verify_aligned_brake_replay(replay, source, recording)


def _write_fixture(name: str, blob: bytes) -> pathlib.Path:
    path = ROOT / "build" / "offline-action-until-release-fixture" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    return path


if __name__ == "__main__":
    unittest.main()
