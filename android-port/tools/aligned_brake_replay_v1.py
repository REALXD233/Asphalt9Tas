#!/usr/bin/env python3
"""Verify a natural A9UER6 steering+brake replay against its A9USR2 source."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from aligned_tick_replay_v1 import (
    AlignedReplayResultV1,
    _snapshot_payload,
    first_frame_alignment_ratios,
)
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    COMMITTED,
    CORRECTION_CORRECTED,
    CORRECTION_EQUAL,
    CORRECTION_MASK,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    STEERING_APPLIED,
)
from parse_unified_executor_report_v5 import FRAME_SIZE, HEADER_SIZE, _FRAME
from parse_unified_executor_report_v6 import (
    BRAKE_APPLIED,
    BRAKE_APPLIED_NATURAL,
    decode_report,
)
from synchronized_brake_recording_v1 import verify_synchronized_brake_capture
from synchronized_action_window_recording_v1 import (
    MAGIC as ACTION_REPORT_MAGIC,
    verify_action_window_capture,
)
from synchronized_action_until_release_recording_v1 import (
    MAGIC as ACTION_RELEASE_MAGIC,
    verify_action_until_release_capture,
)
from unified_tick_recording_v1 import SKIP_BRAKE, SKIP_STEER, decode_recording


REQUIRED_AUDIT_FLAGS = (
    STEERING_APPLIED
    | AUDIT_EXACT
    | GATE2_COMPLETE
    | COMMITTED
    | PREFIX_CERTIFIED
)


def _float_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def verify_aligned_brake_replay(
    replay_report_blob: bytes,
    source_report_blob: bytes,
    recording_blob: bytes,
    accept_startline_direct_audit_flag: bool = False,
) -> AlignedReplayResultV1:
    if source_report_blob[:8] == ACTION_RELEASE_MAGIC:
        source = verify_action_until_release_capture(
            source_report_blob, recording_blob
        ).report
    elif source_report_blob[:8] == ACTION_REPORT_MAGIC:
        source = verify_action_window_capture(source_report_blob, recording_blob)
    else:
        source = verify_synchronized_brake_capture(source_report_blob, recording_blob)
    if source.captured_frames < 2:
        raise ValueError("aligned brake replay requires at least two source frames")
    fixed_interval, inputs = decode_recording(recording_blob)
    summary = decode_report(replay_report_blob)
    if summary.frames != source.captured_frames:
        raise ValueError("brake replay/source frame-count mismatch")
    if summary.first_tick != 0 or summary.last_tick != source.captured_frames - 1:
        raise ValueError("brake replay ticks do not match source")
    if summary.fixed_interval_us != fixed_interval:
        raise ValueError("brake replay fixed interval does not match source")
    if summary.skipped_frames != 0:
        raise ValueError("aligned brake replay may not skip final correction")
    if summary.delta_writes != source.captured_frames:
        raise ValueError("brake replay fixed-delta write count mismatch")
    # Steering and brake share one 64-bit pair at each of the two boundaries.
    if summary.control_writes != source.captured_frames * 2:
        raise ValueError("brake replay pair write count mismatch")

    first_ratios: tuple[float, ...] | None = None
    for index, input_frame in enumerate(inputs):
        if input_frame.skip_flags & (SKIP_STEER | SKIP_BRAKE):
            raise ValueError(f"frame {index}: source did not enable steering+brake")
        frame = _FRAME.unpack_from(
            replay_report_blob, HEADER_SIZE + index * FRAME_SIZE
        )
        flags = frame[3]
        correction = flags & CORRECTION_MASK
        if (flags & REQUIRED_AUDIT_FLAGS) != REQUIRED_AUDIT_FLAGS:
            raise ValueError(f"frame {index}: required brake replay flag is missing")
        brake_flag = flags & (BRAKE_APPLIED | BRAKE_APPLIED_NATURAL)
        if brake_flag == 0:
            raise ValueError(f"frame {index}: brake replay flag is missing")
        if brake_flag == (BRAKE_APPLIED | BRAKE_APPLIED_NATURAL):
            raise ValueError(f"frame {index}: conflicting brake replay flags")
        if flags & BRAKE_APPLIED and not accept_startline_direct_audit_flag:
            raise ValueError(f"frame {index}: legacy Gate 10 brake flag is invalid")
        if correction not in (CORRECTION_EQUAL, CORRECTION_CORRECTED):
            raise ValueError(f"frame {index}: invalid aligned correction mode")
        steering_bits = _float_bits(input_frame.steering)
        brake_bits = _float_bits(input_frame.brake)
        if frame[18] != steering_bits:
            raise ValueError(f"frame {index}: steering target mismatch")
        for label, pair in (("C98", frame[20]), ("C9C", frame[21])):
            if pair >> 32 != steering_bits:
                raise ValueError(f"frame {index}: {label} steering bits mismatch")
            if pair & 0xFFFFFFFF != brake_bits:
                raise ValueError(f"frame {index}: {label} brake bits mismatch")
        if frame[22] != input_frame.transform or frame[23] != input_frame.linear_velocity:
            raise ValueError(f"frame {index}: final payload target mismatch")
        if index == 0:
            before_payload = _snapshot_payload(frame[24])
            first_payload = input_frame.transform + input_frame.linear_velocity
            second_payload = inputs[1].transform + inputs[1].linear_velocity
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
    parser.add_argument(
        "--accept-startline-direct-audit-flag",
        action="store_true",
        help="accept the direct start-line A9UER6 brake audit flag while retaining exact source cross-binding",
    )
    args = parser.parse_args()
    try:
        result = verify_aligned_brake_replay(
            args.replay_report.read_bytes(),
            args.source_report.read_bytes(),
            args.recording.read_bytes(),
            args.accept_startline_direct_audit_flag,
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"aligned_brake_replay_error={error}")
        return 1
    print(
        f"aligned_brake_replay_supported=1 frames={result.frames} "
        f"equal={result.equal_frames} corrected={result.corrected_frames} "
        f"first_alignment_ratio={result.maximum_first_frame_ratio:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
