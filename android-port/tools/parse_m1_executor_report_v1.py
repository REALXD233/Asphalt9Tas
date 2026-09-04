#!/usr/bin/env python3
"""Strict parser for one completed A9M1EX1 executor report."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct


REPORT = struct.Struct("<8s4I14Q2I4i3Q32s32s")
MAGIC = b"A9M1EX1\0"
REQUIRED_FLAGS = 0x1FF


def sha256(path: pathlib.Path) -> bytes:
    return hashlib.sha256(path.read_bytes()).digest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("recording", type=pathlib.Path)
    parser.add_argument("controller_payload", type=pathlib.Path)
    parser.add_argument("--frames", type=int, required=True)
    args = parser.parse_args()
    try:
        blob = args.report.read_bytes()
        if len(blob) != REPORT.size:
            raise ValueError(f"report size is {len(blob)}, expected {REPORT.size}")
        values = REPORT.unpack(blob)
        (magic, version, size, flags, frame_count,
         pid, generation, game_base, main_object, delta_address,
         controller, source, vehicle_owner, event_count, delta_writes,
         thread_additions, read_errors, ptrace_errors, semantic_errors,
         initial_threads, final_threads,
         install_result, completion_result, cleanup_result, delta_result,
         controller_completed, writer_processed, action_completed,
         recording_hash, payload_hash) = values
        if magic != MAGIC or version != 1 or size != REPORT.size:
            raise ValueError("report ABI mismatch")
        if flags != REQUIRED_FLAGS:
            raise ValueError(f"incomplete flags 0x{flags:x}")
        if frame_count != args.frames or not 2 <= frame_count <= 36000:
            raise ValueError("frame count mismatch")
        if any(value == 0 for value in
               (pid, generation, game_base, main_object, delta_address,
                controller, source, vehicle_owner, initial_threads,
                final_threads, event_count, delta_writes)):
            raise ValueError("required runtime identity/count is zero")
        if delta_writes != frame_count:
            raise ValueError("fixed-delta writes must equal authoritative frames")
        if delta_writes > event_count:
            raise ValueError("delta write count exceeds events")
        if (read_errors, ptrace_errors, semantic_errors) != (0, 0, 0):
            raise ValueError("runtime error counter is nonzero")
        if (install_result, completion_result, cleanup_result, delta_result) != (
                0, 0, 0, 0):
            raise ValueError("transaction result is nonzero")
        if (controller_completed, writer_processed, action_completed) != (
                frame_count, frame_count, frame_count):
            raise ValueError("three-way completion receipt mismatch")
        if recording_hash != sha256(args.recording):
            raise ValueError("recording SHA-256 mismatch")
        if payload_hash != sha256(args.controller_payload):
            raise ValueError("controller payload SHA-256 mismatch")
    except (OSError, ValueError, struct.error) as error:
        print(f"M1_EXECUTOR_REPORT_FAIL reason={error}")
        return 1
    print(
        f"M1_EXECUTOR_REPORT_OK frames={frame_count} events={event_count} "
        f"delta_writes={delta_writes} thread_additions={thread_additions} "
        "receipts=controller+writer+action cleanup=controller+writer+detach"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
