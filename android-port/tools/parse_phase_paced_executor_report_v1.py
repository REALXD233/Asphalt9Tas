#!/usr/bin/env python3
"""Strict parser for a completed A9PHEX1 phase-paced executor report."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct


REPORT = struct.Struct("<8s4I24Q2I4i3Q32s32s")
MAGIC = b"A9PHEX1\0"
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
        (
            magic, version, size, flags, frame_count,
            pid, generation, game_base, main_object, delta_address,
            final_owner, c98_address, c9c_address, world_accumulator,
            controller, source, vehicle_owner, event_count, delta_writes,
            thread_additions, read_errors, ptrace_errors, semantic_errors,
            phase_delta_events, phase_c98_events, phase_c9c_events,
            phase_world_events, duplicate_delta_events, committed_frames,
            initial_threads, final_threads,
            install_result, completion_result, cleanup_result, phase_result,
            controller_completed, writer_processed, action_completed,
            recording_hash, payload_hash,
        ) = values
        if magic != MAGIC or version != 1 or size != REPORT.size:
            raise ValueError("report ABI mismatch")
        if flags != REQUIRED_FLAGS:
            raise ValueError(f"incomplete flags 0x{flags:x}")
        if frame_count != args.frames or not 1 <= frame_count <= 36000:
            raise ValueError("frame count mismatch")
        identities = (
            pid, generation, game_base, main_object, delta_address,
            final_owner, c98_address, c9c_address, world_accumulator,
            controller, source, vehicle_owner, initial_threads, final_threads,
            event_count, delta_writes,
        )
        if any(value == 0 for value in identities):
            raise ValueError("required runtime identity/count is zero")
        if (delta_address & 7) != 0 or (c98_address & 3) != 0 or \
                c9c_address != c98_address + 4 or \
                (world_accumulator & 3) != 0:
            raise ValueError("phase watch-address topology mismatch")
        if delta_writes != frame_count or committed_frames != frame_count:
            raise ValueError("authoritative frame/write/commit count mismatch")
        if event_count != (
            phase_delta_events + phase_c98_events + phase_c9c_events +
            phase_world_events
        ):
            raise ValueError("phase event accounting mismatch")
        if phase_delta_events < delta_writes or \
                phase_c98_events < frame_count or \
                phase_c9c_events < frame_count or \
                phase_world_events < frame_count or \
                duplicate_delta_events > phase_delta_events:
            raise ValueError("insufficient or inconsistent phase evidence")
        if (read_errors, ptrace_errors, semantic_errors) != (0, 0, 0):
            raise ValueError("runtime error counter is nonzero")
        if (install_result, completion_result, cleanup_result, phase_result) != (
            0, 0, 0, 0
        ):
            raise ValueError("transaction/phase result is nonzero")
        if (controller_completed, writer_processed, action_completed) != (
            frame_count, frame_count, frame_count
        ):
            raise ValueError("three-way completion receipt mismatch")
        if recording_hash != sha256(args.recording):
            raise ValueError("recording SHA-256 mismatch")
        if payload_hash != sha256(args.controller_payload):
            raise ValueError("controller payload SHA-256 mismatch")
    except (OSError, ValueError, struct.error) as error:
        print(f"PHASE_PACED_EXECUTOR_REPORT_FAIL reason={error}")
        return 1
    print(
        f"PHASE_PACED_EXECUTOR_REPORT_OK frames={frame_count} "
        f"events={event_count} delta_writes={delta_writes} "
        f"duplicate_delta_events={duplicate_delta_events} "
        "boundaries=delta+c98+c9c+world receipts=controller+writer+action "
        "cleanup=controller+writer+detach"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
