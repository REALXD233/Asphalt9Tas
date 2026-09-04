#!/usr/bin/env python3
"""Strict A9USR6 natural-action report and A9UTK1 cross-validator."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from synchronized_brake_recording_v1 import (
    MAGIC as A9USR2_MAGIC,
    VERSION as A9USR2_VERSION,
    REQUIRED_FLAGS as A9USR2_REQUIRED_FLAGS,
    INPUT_CYCLE_ANCHOR_FLAG,
    verify_synchronized_brake_capture,
)
from unified_tick_recording_v1 import (
    FRAME_SIZE,
    HEADER_SIZE as RECORDING_HEADER_SIZE,
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_NITRO,
    SKIP_RESPAWN,
    decode_recording,
)


MAGIC = b"A9USR6\0\0"
VERSION = 6
HEADER_SIZE = 384
BASE_HEADER_SIZE = 240
AUDIT_SIZE = 212
NITRO_CAPTURE_FLAG = 1 << 10
REQUIRED_FLAGS = A9USR2_REQUIRED_FLAGS | INPUT_CYCLE_ANCHOR_FLAG | NITRO_CAPTURE_FLAG
NATURAL_SKIP_FLAGS = (
    SKIP_ACCELERATOR | SKIP_BARREL_ANGULAR | SKIP_BARREL_RBX | SKIP_RESPAWN
)
# The NativeBridge-safe wrapper deliberately performs no gettid syscall.
# Zero is the explicit "not sampled" TID sentinel; call/return/count
# consistency remains the authoritative natural-action receipt.
LAST_TID_NOT_SAMPLED = 0


@dataclass(frozen=True)
class NaturalActionCaptureV1:
    frames: int
    activation_frames: int
    activation_calls: int
    first_activation: int
    last_activation: int


def _u32(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<I", blob, offset)[0]


def _u64(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", blob, offset)[0]


def verify_lifecycle_natural_action_capture(
    report_blob: bytes, recording_blob: bytes,
) -> NaturalActionCaptureV1:
    if len(report_blob) < HEADER_SIZE:
        raise ValueError("report is shorter than the A9USR6 header")
    if report_blob[:8] != MAGIC or _u32(report_blob, 8) != VERSION:
        raise ValueError("unsupported A9USR6 magic/version")
    if _u32(report_blob, 12) != HEADER_SIZE or _u32(report_blob, 16) != AUDIT_SIZE:
        raise ValueError("A9USR6 header/audit ABI mismatch")
    if _u32(report_blob, 20) != REQUIRED_FLAGS:
        raise ValueError("A9USR6 success flags are incomplete")
    captured = _u32(report_blob, 28)
    if len(report_blob) != HEADER_SIZE + captured * AUDIT_SIZE:
        raise ValueError("A9USR6 report length mismatch")

    addresses = struct.unpack_from("<8Q", report_blob, BASE_HEADER_SIZE)
    if any(value == 0 for value in addresses):
        raise ValueError("A9USR6 action address proof is incomplete")
    metrics = struct.unpack_from("<8Q", report_blob, 304)
    (wrapper_entries, original_calls, clean_returns, counted_calls,
     out_of_window, overflow, failures, report_count_sum) = metrics
    session_id, cleanup_passes = struct.unpack_from("<II", report_blob, 368)
    last_status, last_tid = struct.unpack_from("<iI", report_blob, 376)
    if not (wrapper_entries == original_calls == clean_returns == counted_calls):
        raise ValueError("A9USR6 wrapper/original/return counters differ")
    if counted_calls == 0 or counted_calls != report_count_sum:
        raise ValueError("A9USR6 contains no natural activation or count sum differs")
    if out_of_window or overflow or failures:
        raise ValueError("A9USR6 wrapper reported a rejected call")
    if session_id == 0 or not 1 <= cleanup_passes <= 4:
        raise ValueError("A9USR6 session/cleanup proof is invalid")
    if last_status != 1 or last_tid != LAST_TID_NOT_SAMPLED:
        raise ValueError("A9USR6 final natural-call receipt is invalid")

    fixed_interval, frames = decode_recording(recording_blob)
    if fixed_interval != 16667 or len(frames) != captured:
        raise ValueError("A9USR6/A9UTK1 timing or frame count mismatch")
    active = []
    count_sum = 0
    synthetic_recording = bytearray(recording_blob)
    for index, frame in enumerate(frames):
        if frame.skip_flags != NATURAL_SKIP_FLAGS:
            raise ValueError(f"frame {index}: natural-action scope flags mismatch")
        if not 0 <= frame.nitro_activations <= 2:
            raise ValueError(f"frame {index}: Nitro activation count is out of range")
        if frame.nitro_activations:
            active.append(index)
            count_sum += frame.nitro_activations
        offset = RECORDING_HEADER_SIZE + index * FRAME_SIZE
        struct.pack_into("<I", synthetic_recording, offset + 28, 0)
        struct.pack_into("<I", synthetic_recording, offset + 32,
                         frame.skip_flags | SKIP_NITRO)
    if not active or count_sum != report_count_sum:
        raise ValueError("recorded per-frame Nitro counts do not match A9USR6 evidence")

    synthetic_report = bytearray(report_blob[:BASE_HEADER_SIZE])
    synthetic_report[:8] = A9USR2_MAGIC
    struct.pack_into("<I", synthetic_report, 8, A9USR2_VERSION)
    struct.pack_into("<I", synthetic_report, 12, BASE_HEADER_SIZE)
    struct.pack_into("<I", synthetic_report, 20,
                     A9USR2_REQUIRED_FLAGS | INPUT_CYCLE_ANCHOR_FLAG)
    synthetic_report.extend(report_blob[HEADER_SIZE:])
    verify_synchronized_brake_capture(
        bytes(synthetic_report), bytes(synthetic_recording),
        require_input_cycle_anchor=True,
    )
    return NaturalActionCaptureV1(
        frames=captured,
        activation_frames=len(active),
        activation_calls=count_sum,
        first_activation=active[0],
        last_activation=active[-1],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("report", type=Path, nargs="?")
    parser.add_argument("recording", type=Path, nargs="?")
    args = parser.parse_args()
    if args.selftest:
        ok = (HEADER_SIZE == 384 and REQUIRED_FLAGS == 0x67f and
              NATURAL_SKIP_FLAGS == 0x78 and AUDIT_SIZE == 212 and
              LAST_TID_NOT_SAMPLED == 0)
        print(f"a9usr6_selftest_passed={int(ok)}")
        return 0 if ok else 1
    if args.report is None or args.recording is None:
        parser.error("report and recording are required")
    try:
        result = verify_lifecycle_natural_action_capture(
            args.report.read_bytes(), args.recording.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr6_error={error}")
        return 1
    print(
        "a9usr6_supported=1 "
        f"frames={result.frames} activation_frames={result.activation_frames} "
        f"activation_calls={result.activation_calls} "
        f"first_activation={result.first_activation} "
        f"last_activation={result.last_activation} lifecycle_2_to_3=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
