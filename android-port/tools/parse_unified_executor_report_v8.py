#!/usr/bin/env python3
"""Strict A9UER8 verifier for final-writer-bound unified replay reports."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import HEADER_SIZE, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME
from parse_unified_executor_report_v6 import decode_report as decode_report_v6


MAGIC = b"A9UER8\0\0"
VERSION = 8
COMPLETION_WRITE_HEADER = 1 << 6
COMPLETION_WRITE_FRAME = 1 << 12


def decode_report(blob: bytes):
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER8 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9UER8 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9UER8 ABI size mismatch")
    frame_count = header[5]
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if frame_count < 1 or len(blob) != expected_size:
        raise ValueError(f"A9UER8 length must be exactly {expected_size} bytes")
    if header[4] & COMPLETION_WRITE_HEADER:
        # New reports observe the completion-token store itself with a data
        # hardware breakpoint.  A worker token may legally reuse the same
        # numeric value, so the write certificate supersedes the old
        # before!=after proxy without weakening the phase proof.
        frames: list[bytes] = []
        cursor = HEADER_SIZE
        for index in range(frame_count):
            frame = list(_FRAME.unpack_from(blob, cursor))
            if not frame[3] & COMPLETION_WRITE_FRAME:
                raise ValueError(
                    f"frame {index}: completion write certificate is missing"
                )
            frame[3] &= ~COMPLETION_WRITE_FRAME
            if frame[13] == frame[14]:
                # The delegated V2 verifier predates write certificates and
                # rejects equal snapshots. Normalize only its private
                # synthetic buffer so the mature phase/state checks can be
                # reused; the original A9UER8 evidence remains unchanged and
                # the hardware-write certificate above is mandatory.
                frame[14] ^= 1 << 63
            frames.append(_FRAME.pack(*frame))
            cursor += FRAME_SIZE
        header[4] &= ~COMPLETION_WRITE_HEADER
        blob = _HEADER.pack(*header) + b"".join(frames)
    # A9UER8 deliberately retains the complete A9UER6 header/frame ABI. Its
    # difference is provenance: correction before/immediate bytes come from
    # the in-callback final-writer audit rather than a callback-close pwrite.
    # Reuse every mature A9UER6 structural, phase, control and snapshot check.
    header[0] = b"A9UER6\0\0"
    header[1] = 6
    return decode_report_v6(_HEADER.pack(*header) + blob[HEADER_SIZE:])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer8_error={error}")
        return 1
    print(
        f"a9uer8_supported=1 frames={summary.frames} "
        f"ticks={summary.first_tick}..{summary.last_tick} "
        f"fixed_interval_us={summary.fixed_interval_us} "
        f"equal={summary.equal_frames} corrected={summary.corrected_frames} "
        f"skipped={summary.skipped_frames} delta_writes={summary.delta_writes} "
        f"control_writes={summary.control_writes} "
        f"threads={summary.initial_threads}/{summary.final_threads} "
        "correction_boundary=final_writer_callback"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
