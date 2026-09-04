#!/usr/bin/env python3
"""Strict A9USR2 parser and brake-enabled A9UTK1 capture verifier."""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

from synchronized_tick_recording_v1 import (
    HEADER_SIZE,
    RECORDED_SKIP_FLAGS as V1_RECORDED_SKIP_FLAGS,
    REQUIRED_FLAGS as V1_REQUIRED_FLAGS,
    SyncReportV1,
    _HEADER,
    decode_sync_report,
)
from unified_tick_recording_v1 import (
    FRAME_SIZE as RECORDING_FRAME_SIZE,
    HEADER_SIZE as RECORDING_HEADER_SIZE,
    REQUIRED_FRAME_FLAGS,
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_NITRO,
    SKIP_RESPAWN,
    decode_recording,
)


MAGIC = b"A9USR2\0\0"
VERSION = 2
REQUIRED_FLAGS = V1_REQUIRED_FLAGS | (1 << 6)
INPUT_CYCLE_ANCHOR_FLAG = 1 << 9
RECORDED_SKIP_FLAGS = (
    SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
)
assert V1_RECORDED_SKIP_FLAGS == RECORDED_SKIP_FLAGS | (1 << 1)


def _float_from_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def decode_sync_brake_report(
    blob: bytes, *, required_flags: int = REQUIRED_FLAGS
) -> SyncReportV1:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than the A9USR2 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9USR2 magic/version")
    if header[4] != required_flags:
        raise ValueError(f"A9USR2 success flags must be 0x{required_flags:x}")

    # The A9USR2 physical ABI is deliberately identical to A9USR1. Reuse its
    # mature structural verifier after substituting only the semantic envelope.
    synthetic = bytearray(blob)
    synthetic[0:8] = b"A9USR1\0\0"
    struct.pack_into("<I", synthetic, 8, 1)
    struct.pack_into("<I", synthetic, 20, V1_REQUIRED_FLAGS)
    report = decode_sync_report(bytes(synthetic))

    for index, audit in enumerate(report.frames):
        c98_bits = audit.c98_pair_after & 0xFFFFFFFF
        c9c_bits = audit.c9c_pair_after & 0xFFFFFFFF
        if c98_bits != c9c_bits:
            raise ValueError(f"frame {index}: C98/C9C brake bits differ")
        brake = _float_from_bits(c9c_bits)
        if not math.isfinite(brake) or abs(brake) > 1.05:
            raise ValueError(f"frame {index}: captured brake is invalid")
    return report


def verify_synchronized_brake_capture(
    report_blob: bytes, recording_blob: bytes, *,
    require_input_cycle_anchor: bool = False,
) -> SyncReportV1:
    required_flags = REQUIRED_FLAGS
    if require_input_cycle_anchor:
        required_flags |= INPUT_CYCLE_ANCHOR_FLAG
    report = decode_sync_brake_report(
        report_blob, required_flags=required_flags
    )
    fixed_interval_us, frames = decode_recording(recording_blob)
    if len(frames) != report.captured_frames:
        raise ValueError("A9USR2/A9UTK1 frame-count mismatch")
    for index, (audit, frame) in enumerate(zip(report.frames, frames)):
        if audit.applied_delta_us != fixed_interval_us:
            raise ValueError(f"frame {index}: applied delta does not match A9UTK1")
        if frame.tick != audit.tick or frame.monotonic_ns != audit.monotonic_ns:
            raise ValueError(f"frame {index}: tick/timestamp cross-bind mismatch")
        offset = RECORDING_HEADER_SIZE + index * RECORDING_FRAME_SIZE
        steering_bits, brake_bits = struct.unpack_from("<II", recording_blob, offset + 16)
        if steering_bits != audit.steering_bits:
            raise ValueError(f"frame {index}: steering cross-bind mismatch")
        if brake_bits != audit.c9c_pair_after & 0xFFFFFFFF:
            raise ValueError(f"frame {index}: brake cross-bind mismatch")
        if frame.transform != audit.transform or frame.linear_velocity != audit.linear_velocity:
            raise ValueError(f"frame {index}: physics cross-bind mismatch")
        if frame.skip_flags != RECORDED_SKIP_FLAGS:
            raise ValueError(f"frame {index}: brake recorder scope flags mismatch")
        if frame.flags != REQUIRED_FRAME_FLAGS:
            raise ValueError(f"frame {index}: frame semantics flags mismatch")
        if (
            frame.accelerator != 0.0
            or frame.nitro_activations != 0
            or frame.respawn
            or frame.barrel_angular != (0.0, 0.0, 0.0)
            or frame.barrel_rbx != (0.0, 0.0)
        ):
            raise ValueError(f"frame {index}: unproven field is not dormant")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--require-input-cycle-anchor",
        action="store_true",
        help="require the dedicated C98/C9C/delta-zero anchor witness flag",
    )
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        report = verify_synchronized_brake_capture(
            args.report.read_bytes(), args.recording.read_bytes(),
            require_input_cycle_anchor=args.require_input_cycle_anchor,
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr2_error={error}")
        return 1
    print(
        f"a9usr2_supported=1 frames={report.captured_frames} "
        f"ticks=0..{report.captured_frames - 1} events={report.event_count} "
        f"delta_writes={report.delta_writes} "
        f"threads={report.initial_threads}/{report.final_threads} "
        f"input_cycle_anchor={int(args.require_input_cycle_anchor)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
