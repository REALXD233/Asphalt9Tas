#!/usr/bin/env python3
"""Strict A9USR1 parser and A9UTK1 synchronized-capture verifier."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from unified_tick_recording_v1 import (
    HEADER_SIZE as RECORDING_HEADER_SIZE,
    FRAME_SIZE as RECORDING_FRAME_SIZE,
    REQUIRED_FRAME_FLAGS,
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_BRAKE,
    SKIP_NITRO,
    SKIP_RESPAWN,
    SUPPORTED_BUILD_ID,
    decode_recording,
)


MAGIC = b"A9USR1\0\0"
VERSION = 1
HEADER_SIZE = 240
FRAME_AUDIT_SIZE = 212
REQUIRED_FLAGS = 0x3F
RECORDED_SKIP_FLAGS = (
    SKIP_BRAKE
    | SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
)

_HEADER = struct.Struct("<8s6I20sI22Q2I")
_FRAME = struct.Struct("<QQiiqq7Q2QH6s2Q64s12s")
assert _HEADER.size == HEADER_SIZE
assert _FRAME.size == FRAME_AUDIT_SIZE


@dataclass(frozen=True)
class SyncFrameAuditV1:
    tick: int
    monotonic_ns: int
    cycle_tid: int
    commit_tid: int
    original_delta_us: int
    applied_delta_us: int
    events: tuple[int, int, int, int, int, int, int]
    completion_before: int
    completion_after: int
    callback_flags_at_c9c: int
    c98_pair_after: int
    c9c_pair_after: int
    transform: bytes
    linear_velocity: bytes

    @property
    def steering_bits(self) -> int:
        return self.c9c_pair_after >> 32

    @property
    def steering(self) -> float:
        return struct.unpack("<f", struct.pack("<I", self.steering_bits))[0]


@dataclass(frozen=True)
class SyncReportV1:
    target_frames: int
    captured_frames: int
    pid: int
    library_base: int
    main_object: int
    final_owner: int
    physics_context: int
    completion_address: int
    callback_flags_address: int
    f64_address: int
    inner_world: int
    world_accumulator_address: int
    car_physics_state: int
    wrapper: int
    native_body: int
    transform_address: int
    linear_address: int
    event_count: int
    delta_writes: int
    thread_additions: int
    initial_threads: int
    final_threads: int
    frames: tuple[SyncFrameAuditV1, ...]


def _finite_physics(transform: bytes, linear: bytes) -> bool:
    values = struct.unpack("<19f", transform + linear)
    return all(math.isfinite(value) and abs(value) <= 1_000_000.0 for value in values)


def decode_sync_report(blob: bytes) -> SyncReportV1:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than the A9USR1 header")
    values = _HEADER.unpack_from(blob)
    (
        magic,
        version,
        header_size,
        frame_audit_size,
        flags,
        target_frames,
        captured_frames,
        build_id,
        reserved0,
        pid,
        library_base,
        main_object,
        final_owner,
        physics_context,
        completion_address,
        callback_flags_address,
        f64_address,
        inner_world,
        world_accumulator_address,
        car_physics_state,
        wrapper,
        native_body,
        transform_address,
        linear_address,
        event_count,
        delta_writes,
        read_errors,
        ptrace_errors,
        semantic_errors,
        unexpected_stops,
        thread_additions,
        initial_threads,
        final_threads,
    ) = values
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported A9USR1 magic/version")
    if header_size != HEADER_SIZE or frame_audit_size != FRAME_AUDIT_SIZE:
        raise ValueError("A9USR1 ABI size mismatch")
    if flags != REQUIRED_FLAGS:
        raise ValueError(f"A9USR1 success flags must be 0x{REQUIRED_FLAGS:x}")
    if build_id != SUPPORTED_BUILD_ID:
        raise ValueError("A9USR1 build ID mismatch")
    if reserved0 != 0:
        raise ValueError("A9USR1 reserved header field is nonzero")
    if target_frames < 1 or captured_frames != target_frames:
        raise ValueError("A9USR1 capture is incomplete")
    expected_size = HEADER_SIZE + captured_frames * FRAME_AUDIT_SIZE
    if len(blob) != expected_size:
        raise ValueError(f"A9USR1 length must be exactly {expected_size} bytes")
    addresses = (
        pid,
        library_base,
        main_object,
        final_owner,
        physics_context,
        completion_address,
        callback_flags_address,
        f64_address,
        inner_world,
        world_accumulator_address,
        car_physics_state,
        wrapper,
        native_body,
        transform_address,
        linear_address,
    )
    if any(value == 0 for value in addresses):
        raise ValueError("A9USR1 contains a zero identity/address field")
    if transform_address != native_body + 0x10 or linear_address != native_body + 0x150:
        raise ValueError("A9USR1 native physics layout mismatch")
    if any((read_errors, ptrace_errors, semantic_errors, unexpected_stops)):
        raise ValueError("A9USR1 contains a runtime error counter")
    if delta_writes != target_frames:
        raise ValueError("A9USR1 fixed-delta write count mismatch")
    if initial_threads < 1 or final_threads != initial_threads + thread_additions:
        raise ValueError("A9USR1 clean-detach thread accounting mismatch")

    frames: list[SyncFrameAuditV1] = []
    cursor = HEADER_SIZE
    previous_world_event = 0
    previous_time = 0
    for index in range(captured_frames):
        item = _FRAME.unpack_from(blob, cursor)
        frame = SyncFrameAuditV1(
            tick=item[0],
            monotonic_ns=item[1],
            cycle_tid=item[2],
            commit_tid=item[3],
            original_delta_us=item[4],
            applied_delta_us=item[5],
            events=tuple(item[6:13]),
            completion_before=item[13],
            completion_after=item[14],
            callback_flags_at_c9c=item[15],
            c98_pair_after=item[17],
            c9c_pair_after=item[18],
            transform=item[19],
            linear_velocity=item[20],
        )
        if item[16] != bytes(6):
            raise ValueError(f"frame {index}: reserved bytes are nonzero")
        if frame.tick != index:
            raise ValueError(f"frame {index}: tick is not zero-based and contiguous")
        if frame.monotonic_ns == 0 or (index and frame.monotonic_ns < previous_time):
            raise ValueError(f"frame {index}: monotonic timestamp is invalid")
        if frame.cycle_tid <= 0 or frame.commit_tid <= 0 or frame.cycle_tid == frame.commit_tid:
            raise ValueError(f"frame {index}: cycle/commit thread boundary is invalid")
        if not 1 <= frame.original_delta_us <= 1_000_000:
            raise ValueError(f"frame {index}: original delta is invalid")
        if not 1_000 <= frame.applied_delta_us <= 100_000:
            raise ValueError(f"frame {index}: applied delta is invalid")
        if not all(left < right for left, right in zip(frame.events, frame.events[1:])):
            raise ValueError(f"frame {index}: event order is invalid")
        if previous_world_event and not previous_world_event < frame.events[0]:
            raise ValueError(f"frame {index}: event sequence overlaps the previous tick")
        if frame.completion_before == frame.completion_after:
            raise ValueError(f"frame {index}: completion token did not change")
        if frame.callback_flags_at_c9c & 0xFF != 1:
            raise ValueError(f"frame {index}: callback was not open at C9C")
        if not math.isfinite(frame.steering) or abs(frame.steering) > 1.05:
            raise ValueError(f"frame {index}: captured steering is invalid")
        if not _finite_physics(frame.transform, frame.linear_velocity):
            raise ValueError(f"frame {index}: captured physics is invalid")
        frames.append(frame)
        previous_world_event = frame.events[-1]
        previous_time = frame.monotonic_ns
        cursor += FRAME_AUDIT_SIZE
    if event_count < previous_world_event:
        raise ValueError("A9USR1 total event count precedes the final commit")

    return SyncReportV1(
        target_frames,
        captured_frames,
        pid,
        library_base,
        main_object,
        final_owner,
        physics_context,
        completion_address,
        callback_flags_address,
        f64_address,
        inner_world,
        world_accumulator_address,
        car_physics_state,
        wrapper,
        native_body,
        transform_address,
        linear_address,
        event_count,
        delta_writes,
        thread_additions,
        initial_threads,
        final_threads,
        tuple(frames),
    )


def verify_synchronized_capture(report_blob: bytes, recording_blob: bytes) -> SyncReportV1:
    report = decode_sync_report(report_blob)
    fixed_interval_us, recording_frames = decode_recording(recording_blob)
    if len(recording_frames) != report.captured_frames:
        raise ValueError("A9USR1/A9UTK1 frame-count mismatch")
    for index, (audit, frame) in enumerate(zip(report.frames, recording_frames)):
        if audit.applied_delta_us != fixed_interval_us:
            raise ValueError(f"frame {index}: applied delta does not match A9UTK1")
        if frame.tick != audit.tick or frame.monotonic_ns != audit.monotonic_ns:
            raise ValueError(f"frame {index}: tick/timestamp cross-bind mismatch")
        recording_steering_bits = struct.unpack_from(
            "<I", recording_blob,
            RECORDING_HEADER_SIZE + index * RECORDING_FRAME_SIZE + 16,
        )[0]
        if recording_steering_bits != audit.steering_bits:
            raise ValueError(f"frame {index}: steering cross-bind mismatch")
        if frame.transform != audit.transform or frame.linear_velocity != audit.linear_velocity:
            raise ValueError(f"frame {index}: physics cross-bind mismatch")
        if frame.skip_flags != RECORDED_SKIP_FLAGS:
            raise ValueError(f"frame {index}: recorder scope flags mismatch")
        if frame.flags != REQUIRED_FRAME_FLAGS:
            raise ValueError(f"frame {index}: frame semantics flags mismatch")
        if (
            frame.brake != 0.0
            or frame.accelerator != 0.0
            or frame.nitro_activations != 0
            or frame.respawn
            or frame.barrel_angular != (0.0, 0.0, 0.0)
            or frame.barrel_rbx != (0.0, 0.0)
        ):
            raise ValueError(f"frame {index}: unproven field is not dormant")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        report = verify_synchronized_capture(
            args.report.read_bytes(), args.recording.read_bytes()
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr1_error={error}")
        return 1
    print(
        f"a9usr1_supported=1 frames={report.captured_frames} "
        f"ticks=0..{report.captured_frames - 1} events={report.event_count} "
        f"delta_writes={report.delta_writes} "
        f"threads={report.initial_threads}/{report.final_threads}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
