#!/usr/bin/env python3
"""Verify an aligned A9UER5 replay against its synchronized capture source."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    COMMITTED,
    CORRECTION_CORRECTED,
    CORRECTION_EQUAL,
    CORRECTION_MASK,
    CORRECTION_SKIPPED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    STEERING_APPLIED,
    TRANSFORM_OFFSET,
    TRANSFORM_SIZE,
    LINEAR_OFFSET,
    LINEAR_SIZE,
)
from parse_unified_executor_report_v5 import (
    FRAME_SIZE,
    HEADER_SIZE,
    _FRAME,
    decode_report,
)
from synchronized_tick_recording_v1 import verify_synchronized_capture
from unified_tick_recording_v1 import decode_recording


SNAPSHOT_SIZE = 804
REQUIRED_AUDIT_FLAGS = (
    STEERING_APPLIED | AUDIT_EXACT | GATE2_COMPLETE | COMMITTED | PREFIX_CERTIFIED
)


@dataclass(frozen=True)
class AlignedReplayResultV1:
    frames: int
    equal_frames: int
    corrected_frames: int
    maximum_first_frame_ratio: float


def _payload_values(payload: bytes) -> tuple[float, ...]:
    if len(payload) != TRANSFORM_SIZE + LINEAR_SIZE:
        raise ValueError("physics payload must be exactly 64+12 bytes")
    values = struct.unpack("<19f", payload)
    if not all(math.isfinite(value) and abs(value) <= 1_000_000.0 for value in values):
        raise ValueError("physics payload contains a non-finite/out-of-range value")
    return values


def first_frame_alignment_ratios(
    before_payload: bytes, first_payload: bytes, second_payload: bytes
) -> tuple[float, ...]:
    current = _payload_values(before_payload)
    first = _payload_values(first_payload)
    second = _payload_values(second_payload)
    ratios: list[float] = []
    for index, (value, target, following) in enumerate(zip(current, first, second)):
        step = abs(following - target)
        floor = 0.05 if index < 16 else 2.0
        tolerance = max(floor, step * 8.0 + 0.001)
        ratios.append(abs(value - target) / tolerance)
    return tuple(ratios)


def first_frame_aligned(
    before_payload: bytes, first_payload: bytes, second_payload: bytes
) -> bool:
    return max(first_frame_alignment_ratios(before_payload, first_payload, second_payload)) <= 1.0


def _snapshot_payload(snapshot: bytes) -> bytes:
    if len(snapshot) != SNAPSHOT_SIZE:
        raise ValueError("A9UER5 snapshot size mismatch")
    return (
        snapshot[TRANSFORM_OFFSET : TRANSFORM_OFFSET + TRANSFORM_SIZE]
        + snapshot[LINEAR_OFFSET : LINEAR_OFFSET + LINEAR_SIZE]
    )


def verify_aligned_replay(
    replay_report_blob: bytes,
    source_report_blob: bytes,
    recording_blob: bytes,
) -> AlignedReplayResultV1:
    source = verify_synchronized_capture(source_report_blob, recording_blob)
    if source.captured_frames < 2:
        raise ValueError("aligned replay requires at least two synchronized frames")
    fixed_interval, recording_frames = decode_recording(recording_blob)
    summary = decode_report(replay_report_blob)
    if summary.frames != source.captured_frames:
        raise ValueError("replay/source frame-count mismatch")
    if summary.first_tick != 0 or summary.last_tick != source.captured_frames - 1:
        raise ValueError("replay ticks do not match synchronized source")
    if summary.fixed_interval_us != fixed_interval:
        raise ValueError("replay fixed interval does not match source")
    if summary.skipped_frames != 0:
        raise ValueError("aligned replay may not skip final correction")
    if summary.delta_writes != source.captured_frames:
        raise ValueError("aligned replay fixed-delta write count mismatch")
    if summary.control_writes != source.captured_frames * 2:
        raise ValueError("aligned replay steering write count mismatch")

    first_ratios: tuple[float, ...] | None = None
    for index, input_frame in enumerate(recording_frames):
        frame = _FRAME.unpack_from(
            replay_report_blob, HEADER_SIZE + index * FRAME_SIZE
        )
        flags = frame[3]
        correction = flags & CORRECTION_MASK
        if (flags & REQUIRED_AUDIT_FLAGS) != REQUIRED_AUDIT_FLAGS:
            raise ValueError(f"frame {index}: required replay audit flag is missing")
        if correction not in (CORRECTION_EQUAL, CORRECTION_CORRECTED):
            raise ValueError(f"frame {index}: invalid aligned correction mode")
        steering_bits = struct.unpack("<I", struct.pack("<f", input_frame.steering))[0]
        if frame[18] != steering_bits:
            raise ValueError(f"frame {index}: steering target mismatch")
        if frame[20] >> 32 != steering_bits or frame[21] >> 32 != steering_bits:
            raise ValueError(f"frame {index}: steering write audit mismatch")
        if frame[22] != input_frame.transform or frame[23] != input_frame.linear_velocity:
            raise ValueError(f"frame {index}: final payload target mismatch")
        if index == 0:
            before_payload = _snapshot_payload(frame[24])
            first_payload = input_frame.transform + input_frame.linear_velocity
            second = recording_frames[1]
            second_payload = second.transform + second.linear_velocity
            first_ratios = first_frame_alignment_ratios(
                before_payload, first_payload, second_payload
            )
            if max(first_ratios) > 1.0:
                raise ValueError("first frame exceeded the alignment guard")
    assert first_ratios is not None
    return AlignedReplayResultV1(
        source.captured_frames,
        summary.equal_frames,
        summary.corrected_frames,
        max(first_ratios),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay_report", type=Path)
    parser.add_argument("source_report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        result = verify_aligned_replay(
            args.replay_report.read_bytes(),
            args.source_report.read_bytes(),
            args.recording.read_bytes(),
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"aligned_replay_error={error}")
        return 1
    print(
        f"aligned_replay_supported=1 frames={result.frames} "
        f"equal={result.equal_frames} corrected={result.corrected_frames} "
        f"first_alignment_ratio={result.maximum_first_frame_ratio:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
