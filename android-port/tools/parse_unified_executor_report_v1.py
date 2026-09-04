#!/usr/bin/env python3
"""Strict offline verifier for successful A9UER1 unified-executor reports."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"A9UER1\0\0"
VERSION = 1
HEADER_SIZE = 296
FRAME_SIZE = 1764
BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
REQUIRED_HEADER_FLAGS = 0x1F

STEERING_APPLIED = 1 << 0
CORRECTION_EQUAL = 1 << 1
CORRECTION_CORRECTED = 1 << 2
CORRECTION_SKIPPED = 1 << 3
AUDIT_EXACT = 1 << 4
GATE2_COMPLETE = 1 << 5
COMMITTED = 1 << 6
SUPPORTED_FRAME_FLAGS = 0x7F
CORRECTION_MASK = CORRECTION_EQUAL | CORRECTION_CORRECTED | CORRECTION_SKIPPED
REQUIRED_FRAME_FLAGS = AUDIT_EXACT | GATE2_COMPLETE | COMMITTED

NATIVE_SIZE = 0x290
SNAPSHOT_SIZE = 804
TRANSFORM_OFFSET = 0x10
TRANSFORM_SIZE = 64
LINEAR_OFFSET = 0x150
LINEAR_SIZE = 12

_HEADER = struct.Struct("<8s6I20sI29Q2I")
_FRAME = struct.Struct("<QQiIqq5Q64s12s804s804s")
assert _HEADER.size == HEADER_SIZE
assert _FRAME.size == FRAME_SIZE


@dataclass(frozen=True)
class UnifiedReportSummaryV1:
    frames: int
    first_tick: int
    last_tick: int
    fixed_interval_us: int
    equal_frames: int
    corrected_frames: int
    skipped_frames: int
    delta_writes: int
    control_writes: int
    initial_threads: int
    final_threads: int


def _component_equal(left: bytes, right: bytes) -> bool:
    left_values = struct.unpack("<19f", left)
    right_values = struct.unpack("<19f", right)
    return all(a == b for a, b in zip(left_values, right_values))


def _finite_payload(payload: bytes) -> bool:
    return all(math.isfinite(value) and abs(value) <= 1_000_000.0 for value in struct.unpack("<19f", payload))


def decode_report(blob: bytes) -> UnifiedReportSummaryV1:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER1 header")
    unpacked = _HEADER.unpack_from(blob)
    (
        magic,
        version,
        header_size,
        frame_size,
        flags,
        recording_frames,
        processed_frames,
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
        control_writes,
        equal_frames,
        corrected_frames,
        skipped_frames,
        read_errors,
        ptrace_errors,
        semantic_errors,
        write_attempts,
        write_failures,
        rollback_attempts,
        rollback_failures,
        thread_additions,
        initial_threads,
        final_threads,
    ) = unpacked
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported A9UER1 magic/version")
    if header_size != HEADER_SIZE or frame_size != FRAME_SIZE:
        raise ValueError("A9UER1 ABI size mismatch")
    if flags != REQUIRED_HEADER_FLAGS or reserved0 != 0:
        raise ValueError("A9UER1 flags/reserved mismatch")
    if build_id != BUILD_ID:
        raise ValueError("A9UER1 Build ID mismatch")
    if recording_frames < 1 or processed_frames != recording_frames:
        raise ValueError("A9UER1 incomplete frame count")
    expected_size = HEADER_SIZE + recording_frames * FRAME_SIZE
    if len(blob) != expected_size:
        raise ValueError(f"A9UER1 length must be exactly {expected_size} bytes")
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
        raise ValueError("A9UER1 contains a zero identity/address")
    if transform_address != native_body + TRANSFORM_OFFSET or linear_address != native_body + LINEAR_OFFSET:
        raise ValueError("A9UER1 native payload address mismatch")
    if any((read_errors, ptrace_errors, semantic_errors, write_failures, rollback_attempts, rollback_failures)):
        raise ValueError("A9UER1 success report contains an error/rollback")
    if equal_frames + corrected_frames + skipped_frames != recording_frames:
        raise ValueError("A9UER1 correction counters do not cover all frames")
    if write_attempts != corrected_frames:
        raise ValueError("A9UER1 write-attempt count mismatch")
    if delta_writes < recording_frames:
        raise ValueError("A9UER1 fixed-delta write count is too small")
    if initial_threads < 1 or final_threads != initial_threads + thread_additions:
        raise ValueError("A9UER1 thread detach accounting mismatch")

    previous_tick: int | None = None
    previous_time: int | None = None
    previous_commit = 0
    fixed_interval: int | None = None
    counted_equal = counted_corrected = counted_skipped = 0
    counted_steering = 0
    cursor = HEADER_SIZE
    for index in range(recording_frames):
        (
            tick,
            monotonic_ns,
            cycle_tid,
            frame_flags,
            original_delta_us,
            applied_delta_us,
            completion_event,
            callback_open_event,
            f64_event,
            callback_close_event,
            world_commit_event,
            recorded_transform,
            recorded_linear,
            before,
            immediate,
        ) = _FRAME.unpack_from(blob, cursor)
        if cycle_tid <= 0:
            raise ValueError(f"frame {index}: invalid cycle tid")
        if frame_flags & ~SUPPORTED_FRAME_FLAGS or (frame_flags & REQUIRED_FRAME_FLAGS) != REQUIRED_FRAME_FLAGS:
            raise ValueError(f"frame {index}: invalid audit flags")
        correction = frame_flags & CORRECTION_MASK
        if correction not in (CORRECTION_EQUAL, CORRECTION_CORRECTED, CORRECTION_SKIPPED):
            raise ValueError(f"frame {index}: correction mode is not unique")
        if previous_tick is not None and tick != previous_tick + 1:
            raise ValueError(f"frame {index}: tick is not contiguous")
        if previous_time is not None and monotonic_ns < previous_time:
            raise ValueError(f"frame {index}: monotonic time moved backward")
        if not 0 < original_delta_us <= 1_000_000 or not 1000 <= applied_delta_us <= 100_000:
            raise ValueError(f"frame {index}: invalid delta")
        if fixed_interval is None:
            fixed_interval = applied_delta_us
        elif applied_delta_us != fixed_interval:
            raise ValueError(f"frame {index}: fixed delta changed")
        events = (completion_event, callback_open_event, f64_event, callback_close_event, world_commit_event)
        if not (previous_commit < events[0] < events[1] < events[2] < events[3] < events[4] <= event_count):
            raise ValueError(f"frame {index}: Gate 2 event order mismatch")

        recorded_payload = recorded_transform + recorded_linear
        before_payload = before[TRANSFORM_OFFSET : TRANSFORM_OFFSET + TRANSFORM_SIZE] + before[LINEAR_OFFSET : LINEAR_OFFSET + LINEAR_SIZE]
        if not _finite_payload(recorded_payload) or not _finite_payload(before_payload):
            raise ValueError(f"frame {index}: non-finite physics payload")
        equal = _component_equal(before_payload, recorded_payload)
        if correction == CORRECTION_EQUAL:
            counted_equal += 1
            if not equal or immediate != before:
                raise ValueError(f"frame {index}: equal-frame audit mismatch")
        elif correction == CORRECTION_CORRECTED:
            counted_corrected += 1
            if equal:
                raise ValueError(f"frame {index}: corrected frame was already equal")
            expected = bytearray(before)
            expected[TRANSFORM_OFFSET : TRANSFORM_OFFSET + TRANSFORM_SIZE] = recorded_transform
            expected[LINEAR_OFFSET : LINEAR_OFFSET + LINEAR_SIZE] = recorded_linear
            if immediate != bytes(expected):
                raise ValueError(f"frame {index}: corrected-frame audit mismatch")
        else:
            counted_skipped += 1
            if immediate != before:
                raise ValueError(f"frame {index}: skipped frame changed gameplay state")
        if frame_flags & STEERING_APPLIED:
            counted_steering += 1
        previous_tick = tick
        previous_time = monotonic_ns
        previous_commit = world_commit_event
        cursor += FRAME_SIZE

    if (counted_equal, counted_corrected, counted_skipped) != (equal_frames, corrected_frames, skipped_frames):
        raise ValueError("A9UER1 header/frame correction count mismatch")
    if control_writes != counted_steering * 2:
        raise ValueError("A9UER1 steering write count mismatch")
    assert fixed_interval is not None and previous_tick is not None
    return UnifiedReportSummaryV1(
        recording_frames,
        _FRAME.unpack_from(blob, HEADER_SIZE)[0],
        previous_tick,
        fixed_interval,
        equal_frames,
        corrected_frames,
        skipped_frames,
        delta_writes,
        control_writes,
        initial_threads,
        final_threads,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer1_error={error}")
        return 1
    print(
        f"a9uer1_supported=1 frames={summary.frames} "
        f"ticks={summary.first_tick}..{summary.last_tick} "
        f"fixed_interval_us={summary.fixed_interval_us} "
        f"equal={summary.equal_frames} corrected={summary.corrected_frames} "
        f"skipped={summary.skipped_frames} delta_writes={summary.delta_writes} "
        f"control_writes={summary.control_writes} "
        f"threads={summary.initial_threads}/{summary.final_threads}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
