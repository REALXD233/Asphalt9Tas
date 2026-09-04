#!/usr/bin/env python3
"""A9USR3 host-triggered neutral-anchor action-window tests."""

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
from synchronized_action_window_recording_v1 import (
    MAGIC,
    REQUIRED_FLAGS,
    VERSION,
    verify_action_window_capture,
)
from synchronized_tick_recording_v1 import (
    FRAME_AUDIT_SIZE,
    HEADER_SIZE as REPORT_HEADER_SIZE,
    _FRAME as REPORT_FRAME,
    _HEADER as REPORT_HEADER,
)
from test_synchronized_brake_recording_v1 import make_brake_source
from unified_tick_recording_v1 import (
    FRAME_SIZE,
    HEADER_SIZE,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)
from verify_action_window_anchor_v1 import verify as verify_anchor


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)
ANCHOR = ROOT / "evidence" / "a9tas_natural_preroll_source_run1.a9npa1"
REPLAY = ROOT / "evidence" / "a9tas_natural_preroll_full_replay_run1_20260817.a9uer5"


def make_action_window(frame_count: int = 5) -> tuple[bytes, bytes]:
    if frame_count < 5:
        raise ValueError("action-window fixture requires at least five frames")
    report_blob, recording_blob = make_brake_source(0)
    if frame_count == 5:
        report_header = list(REPORT_HEADER.unpack_from(report_blob))
        report_header[0], report_header[1], report_header[4] = (
            MAGIC,
            VERSION,
            REQUIRED_FLAGS,
        )
        recording = bytearray(recording_blob)
        report_frames: list[bytes] = []
        pattern = (
            (0.0, 0.0),
            (0.5, -1.0),
            (0.5, -1.0),
            (0.5, -1.0),
            (0.0, 0.0),
        )
        for index, (steering, brake) in enumerate(pattern):
            steering_bits = struct.unpack("<I", struct.pack("<f", steering))[0]
            brake_bits = struct.unpack("<I", struct.pack("<f", brake))[0]
            offset = HEADER_SIZE + index * FRAME_SIZE
            struct.pack_into("<II", recording, offset + 16, steering_bits, brake_bits)
            frame = list(
                REPORT_FRAME.unpack_from(
                    report_blob, REPORT_HEADER_SIZE + index * FRAME_AUDIT_SIZE
                )
            )
            pair = (steering_bits << 32) | brake_bits
            frame[17] = pair
            frame[18] = pair
            report_frames.append(REPORT_FRAME.pack(*frame))
        return (
            REPORT_HEADER.pack(*report_header) + b"".join(report_frames),
            bytes(recording),
        )

    report_header = list(REPORT_HEADER.unpack_from(report_blob))
    report_header[0], report_header[1], report_header[4] = MAGIC, VERSION, REQUIRED_FLAGS
    report_header[5] = report_header[6] = frame_count
    report_header[25] = frame_count
    fixed_interval_us, base_recording_frames = decode_recording(recording_blob)
    base_frame = base_recording_frames[0]
    base_audit = list(REPORT_FRAME.unpack_from(report_blob, REPORT_HEADER_SIZE))
    first_event = base_audit[6]
    first_time = base_audit[1]
    recording_frames: list[UnifiedTickFrameV1] = []
    report_frames: list[bytes] = []
    active_start = max(1, frame_count // 6)
    active_end = max(active_start + 1, frame_count // 2)
    for index in range(frame_count):
        steering, brake = (
            (0.5, -1.0)
            if active_start <= index < active_end
            else (0.0, 0.0)
        )
        steering_bits = struct.unpack("<I", struct.pack("<f", steering))[0]
        brake_bits = struct.unpack("<I", struct.pack("<f", brake))[0]
        monotonic_ns = first_time + index * fixed_interval_us * 1000
        recording_frames.append(
            UnifiedTickFrameV1(
                index,
                monotonic_ns,
                steering,
                brake,
                base_frame.accelerator,
                base_frame.nitro_activations,
                base_frame.skip_flags,
                base_frame.respawn,
                base_frame.barrel_angular,
                base_frame.barrel_rbx,
                base_frame.transform,
                base_frame.linear_velocity,
                base_frame.flags,
            )
        )
        frame = base_audit.copy()
        frame[0] = index
        frame[1] = monotonic_ns
        frame[6:13] = tuple(
            first_event + index * 7 + event_offset for event_offset in range(7)
        )
        frame[13] = base_audit[13] + index
        frame[14] = base_audit[14] + index
        pair = (steering_bits << 32) | brake_bits
        frame[17] = pair
        frame[18] = pair
        report_frames.append(REPORT_FRAME.pack(*frame))
    report_header[24] = first_event + (frame_count - 1) * 7 + 6
    return (
        REPORT_HEADER.pack(*report_header) + b"".join(report_frames),
        encode_recording(recording_frames, fixed_interval_us=fixed_interval_us),
    )


class SynchronizedActionWindowRecordingTests(unittest.TestCase):
    def test_source_requires_absent_then_observed_host_trigger_and_neutral_pair(self) -> None:
        self.assertIn("A9TAS_SYNC_ACTION_WINDOW_V1", SOURCE)
        self.assertIn("stale action-window trigger exists", SOURCE)
        self.assertIn("access(action_start_path, F_OK) == 0", SOURCE)
        self.assertIn("pending_audit.c98_pair_after == 0", SOURCE)
        self.assertIn("pending_audit.c9c_pair_after == 0", SOURCE)
        self.assertIn("kSyncHostTriggerObserved", SOURCE)
        self.assertIn("unlink(action_start_path)", SOURCE)

    def test_accepts_neutral_lead_in_combined_hold_and_release(self) -> None:
        report, recording = make_action_window()
        parsed = verify_action_window_capture(report, recording)
        self.assertEqual(parsed.captured_frames, 5)
        verify_anchor(report, recording, ANCHOR.read_bytes())

    def test_accepts_full_180_frame_action_window(self) -> None:
        report, recording = make_action_window(180)
        parsed = verify_action_window_capture(report, recording)
        self.assertEqual(parsed.captured_frames, 180)

    def test_rejects_non_neutral_frame0_and_missing_release(self) -> None:
        report, recording = make_action_window()
        changed_report = bytearray(report)
        changed = bytearray(recording)
        struct.pack_into("<I", changed, HEADER_SIZE + 20, 0xBF800000)
        first = list(REPORT_FRAME.unpack_from(changed_report, REPORT_HEADER_SIZE))
        first[17] = 0xBF800000
        first[18] = 0xBF800000
        changed_report[
            REPORT_HEADER_SIZE : REPORT_HEADER_SIZE + FRAME_AUDIT_SIZE
        ] = REPORT_FRAME.pack(*first)
        with self.assertRaisesRegex(ValueError, "frame 0 must preserve a neutral"):
            verify_action_window_capture(bytes(changed_report), bytes(changed))

        changed_report = bytearray(report)
        changed_recording = bytearray(recording)
        for index in (4,):
            pair = (0x3F000000 << 32) | 0xBF800000
            frame = list(REPORT_FRAME.unpack_from(
                changed_report, REPORT_HEADER_SIZE + index * FRAME_AUDIT_SIZE
            ))
            frame[17] = pair
            frame[18] = pair
            changed_report[
                REPORT_HEADER_SIZE + index * FRAME_AUDIT_SIZE :
                REPORT_HEADER_SIZE + (index + 1) * FRAME_AUDIT_SIZE
            ] = REPORT_FRAME.pack(*frame)
            offset = HEADER_SIZE + index * FRAME_SIZE
            struct.pack_into("<II", changed_recording, offset + 16, 0x3F000000, 0xBF800000)
        with self.assertRaisesRegex(ValueError, "lacks brake release"):
            verify_action_window_capture(bytes(changed_report), bytes(changed_recording))

    def test_combined_replay_accepts_a9usr3_source(self) -> None:
        source, recording = make_action_window()
        replay_blob = REPLAY.read_bytes()
        header = list(REPLAY_HEADER.unpack_from(replay_blob))
        header[0], header[1] = b"A9UER6\0\0", 6
        replay_frames: list[bytes] = []
        pattern = ((0.0, 0.0), (0.5, -1.0), (0.5, -1.0), (0.5, -1.0), (0.0, 0.0))
        for index, (steering, brake) in enumerate(pattern):
            steering_bits = struct.unpack("<I", struct.pack("<f", steering))[0]
            brake_bits = struct.unpack("<I", struct.pack("<f", brake))[0]
            frame = list(REPLAY_FRAME.unpack_from(
                replay_blob, REPLAY_HEADER_SIZE + index * REPLAY_FRAME_SIZE
            ))
            frame[3] |= BRAKE_APPLIED_NATURAL
            frame[18] = steering_bits
            frame[20] = (steering_bits << 32) | brake_bits
            frame[21] = (steering_bits << 32) | brake_bits
            replay_frames.append(REPLAY_FRAME.pack(*frame))
        replay = REPLAY_HEADER.pack(*header) + b"".join(replay_frames)
        result = verify_aligned_brake_replay(replay, source, recording)
        self.assertEqual(result.frames, 5)


if __name__ == "__main__":
    unittest.main()
