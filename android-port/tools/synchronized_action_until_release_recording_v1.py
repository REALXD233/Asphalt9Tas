#!/usr/bin/env python3
"""Strict A9USR4 verifier for a bounded action-until-release capture."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from synchronized_action_window_recording_v1 import (
    MAGIC as ACTION_WINDOW_MAGIC,
    REQUIRED_FLAGS as ACTION_WINDOW_REQUIRED_FLAGS,
    VERSION as ACTION_WINDOW_VERSION,
    verify_action_window_capture,
)
from synchronized_tick_recording_v1 import SyncReportV1, _HEADER
from unified_tick_recording_v1 import decode_recording


MAGIC = b"A9USR4\0\0"
VERSION = 4
REQUIRED_FLAGS = ACTION_WINDOW_REQUIRED_FLAGS | (1 << 8)
POST_RELEASE_FRAMES = 30


@dataclass(frozen=True)
class ActionUntilReleaseV1:
    maximum_frames: int
    release_index: int
    report: SyncReportV1


def _as_fixed_action_window(blob: bytes) -> tuple[bytes, int, int]:
    if len(blob) < _HEADER.size:
        raise ValueError("report is shorter than the A9USR4 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9USR4 magic/version")
    if header[4] != REQUIRED_FLAGS:
        raise ValueError(f"A9USR4 success flags must be 0x{REQUIRED_FLAGS:x}")
    maximum_frames, captured_frames = header[5], header[6]
    if not POST_RELEASE_FRAMES <= captured_frames <= maximum_frames:
        raise ValueError("A9USR4 captured frame count exceeds its bounded maximum")
    synthetic = bytearray(blob)
    synthetic[0:8] = ACTION_WINDOW_MAGIC
    struct.pack_into("<I", synthetic, 8, ACTION_WINDOW_VERSION)
    struct.pack_into("<I", synthetic, 20, ACTION_WINDOW_REQUIRED_FLAGS)
    struct.pack_into("<I", synthetic, 24, captured_frames)
    return bytes(synthetic), maximum_frames, captured_frames


def decode_action_until_release_report(blob: bytes) -> ActionUntilReleaseV1:
    synthetic, maximum_frames, captured_frames = _as_fixed_action_window(blob)
    # The semantic verifier below performs the source cross-binding. This
    # structural pass provides the decoded identities for A9NPA1 binding.
    from synchronized_action_window_recording_v1 import decode_action_window_report

    report = decode_action_window_report(synthetic)
    if report.captured_frames != captured_frames:
        raise ValueError("A9USR4 normalized frame count mismatch")
    return ActionUntilReleaseV1(maximum_frames, -1, report)


def verify_action_until_release_capture(
    report_blob: bytes, recording_blob: bytes
) -> ActionUntilReleaseV1:
    synthetic, maximum_frames, _ = _as_fixed_action_window(report_blob)
    report = verify_action_window_capture(synthetic, recording_blob)
    _, frames = decode_recording(recording_blob)
    active = next(
        index
        for index, frame in enumerate(frames)
        if frame.brake <= -0.5 and abs(frame.steering) >= 0.1
    )
    release = next(
        index
        for index, frame in enumerate(frames[active + 1 :], active + 1)
        if frame.brake == 0.0
    )
    if len(frames) - release != POST_RELEASE_FRAMES:
        raise ValueError(
            f"A9USR4 must end after exactly {POST_RELEASE_FRAMES} release/post-roll frames"
        )
    return ActionUntilReleaseV1(maximum_frames, release, report)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        result = verify_action_until_release_capture(
            args.report.read_bytes(), args.recording.read_bytes()
        )
    except (OSError, StopIteration, ValueError, struct.error) as error:
        print(f"a9usr4_error={error}")
        return 1
    print(
        f"a9usr4_supported=1 frames={result.report.captured_frames}/"
        f"{result.maximum_frames} release={result.release_index} "
        f"delta_writes={result.report.delta_writes} "
        f"threads={result.report.initial_threads}/{result.report.final_threads}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
