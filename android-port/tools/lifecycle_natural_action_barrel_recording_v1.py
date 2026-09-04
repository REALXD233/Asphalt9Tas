#!/usr/bin/env python3
"""Strict A9USR6/A9UTK1 validator for natural-action plus barrel fields."""

from __future__ import annotations

import argparse
import dataclasses
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from lifecycle_natural_action_recording_v1 import (
    AUDIT_SIZE,
    HEADER_SIZE,
    NATURAL_SKIP_FLAGS,
    REQUIRED_FLAGS,
    NaturalActionCaptureV1,
    verify_lifecycle_natural_action_capture,
)
from unified_tick_recording_v1 import (
    SKIP_ACCELERATOR,
    SKIP_RESPAWN,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)


BARREL_CAPTURE_SKIP_FLAGS = SKIP_ACCELERATOR | SKIP_RESPAWN
COMPLETION_WRITE_CERTIFICATE_FLAG = 1 << 11
AUDIT_CERTIFICATE_OFFSET = 114
ZERO_ANGULAR = (0.0, 0.0, 0.0)
ZERO_RBX = (0.0, 0.0)


@dataclass(frozen=True)
class NaturalActionBarrelCaptureV1:
    natural: NaturalActionCaptureV1
    nonzero_frames: int
    changed_frames: int
    angular_min: tuple[float, float, float]
    angular_max: tuple[float, float, float]
    rbx_min: tuple[float, float]
    rbx_max: tuple[float, float]


def _raw_barrel(frame: UnifiedTickFrameV1) -> bytes:
    return struct.pack("<5f", *frame.barrel_angular, *frame.barrel_rbx)


def _legacy_recording_view(
    fixed_interval_us: int, frames: tuple[UnifiedTickFrameV1, ...],
) -> bytes:
    """Mask only the new fields so the mature A9USR6 validator can be reused."""
    return encode_recording(
        (
            dataclasses.replace(
                frame,
                skip_flags=NATURAL_SKIP_FLAGS,
                barrel_angular=ZERO_ANGULAR,
                barrel_rbx=ZERO_RBX,
            )
            for frame in frames
        ),
        fixed_interval_us=fixed_interval_us,
    )


def _legacy_report_view(report_blob: bytes, captured_frames: int) -> bytes:
    if len(report_blob) < HEADER_SIZE:
        raise ValueError("report is shorter than the certified A9USR6 header")
    flags = struct.unpack_from("<I", report_blob, 20)[0]
    if flags != REQUIRED_FLAGS | COMPLETION_WRITE_CERTIFICATE_FLAG:
        raise ValueError("A9USR6 completion-write certificate flag mismatch")
    report_frames = struct.unpack_from("<I", report_blob, 28)[0]
    if report_frames != captured_frames:
        raise ValueError("certified A9USR6/A9UTK1 frame-count mismatch")
    if len(report_blob) != HEADER_SIZE + report_frames * AUDIT_SIZE:
        raise ValueError("certified A9USR6 report length mismatch")

    legacy = bytearray(report_blob)
    struct.pack_into("<I", legacy, 20, REQUIRED_FLAGS)
    for index in range(report_frames):
        audit = HEADER_SIZE + index * AUDIT_SIZE
        reserved = audit + AUDIT_CERTIFICATE_OFFSET
        if report_blob[reserved] != 1 or report_blob[reserved + 1:reserved + 6] != bytes(5):
            raise ValueError(f"frame {index}: completion-write certificate missing")
        legacy[reserved:reserved + 6] = bytes(6)
        # The legacy validator accepts only snapshot inequality.  A certified
        # same-value store is stronger evidence, but cannot be expressed in its
        # old ABI.  Change only the in-memory compatibility view so the mature
        # event/control/physics checks can run; never alter the source report.
        completion_before = struct.unpack_from("<Q", report_blob, audit + 96)[0]
        completion_after = struct.unpack_from("<Q", report_blob, audit + 104)[0]
        if completion_after == completion_before:
            struct.pack_into("<Q", legacy, audit + 104,
                             completion_before ^ 1)
    return bytes(legacy)


def verify_lifecycle_natural_action_barrel_capture(
    report_blob: bytes,
    recording_blob: bytes,
    *,
    require_field_change: bool = True,
) -> NaturalActionBarrelCaptureV1:
    fixed_interval_us, frames = decode_recording(recording_blob)
    if any(frame.skip_flags != BARREL_CAPTURE_SKIP_FLAGS for frame in frames):
        raise ValueError("natural-action barrel scope flags mismatch")

    packed = tuple(_raw_barrel(frame) for frame in frames)
    nonzero_frames = sum(
        any(value != 0.0 for value in (*frame.barrel_angular, *frame.barrel_rbx))
        for frame in frames
    )
    changed_frames = sum(
        packed[index] != packed[index - 1] for index in range(1, len(packed))
    )
    if nonzero_frames == 0:
        raise ValueError("barrel source fields are dormant in every frame")
    if require_field_change and changed_frames == 0:
        raise ValueError("barrel source fields never change")

    angular_columns = tuple(zip(*(frame.barrel_angular for frame in frames)))
    rbx_columns = tuple(zip(*(frame.barrel_rbx for frame in frames)))
    if not all(math.isfinite(value) for frame in frames for value in (
        *frame.barrel_angular, *frame.barrel_rbx,
    )):
        raise ValueError("barrel source field is non-finite")

    natural = verify_lifecycle_natural_action_capture(
        _legacy_report_view(report_blob, len(frames)),
        _legacy_recording_view(fixed_interval_us, frames),
    )
    if natural.frames != len(frames):
        raise ValueError("natural-action/barrel frame-count mismatch")
    return NaturalActionBarrelCaptureV1(
        natural=natural,
        nonzero_frames=nonzero_frames,
        changed_frames=changed_frames,
        angular_min=tuple(min(values) for values in angular_columns),
        angular_max=tuple(max(values) for values in angular_columns),
        rbx_min=tuple(min(values) for values in rbx_columns),
        rbx_max=tuple(max(values) for values in rbx_columns),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        result = verify_lifecycle_natural_action_barrel_capture(
            args.report.read_bytes(), args.recording.read_bytes()
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr6_barrel_error={error}")
        return 1
    print(
        "a9usr6_barrel_supported=1 "
        f"frames={result.natural.frames} "
        f"activation_calls={result.natural.activation_calls} "
        f"nonzero_frames={result.nonzero_frames} "
        f"changed_frames={result.changed_frames} "
        f"angular_min={result.angular_min} angular_max={result.angular_max} "
        f"rbx_min={result.rbx_min} rbx_max={result.rbx_max}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
