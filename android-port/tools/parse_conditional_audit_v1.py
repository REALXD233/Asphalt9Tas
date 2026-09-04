#!/usr/bin/env python3
"""Fail-closed parser for A9CDT1 conditional correction audit reports."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"A9CDT1\0\0"
VERSION = 1
SUPPORTED_BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
REQUIRED_HEADER_FLAGS = 0xF
FRAME_EQUAL = 1 << 0
FRAME_CORRECTED = 1 << 1
FRAME_AUDIT_EXACT = 1 << 2
SNAPSHOT_SIZE = 804
HEADER = struct.Struct("<8s8I20sI17QII")
FRAME_PREFIX = struct.Struct("<QQII64s12s")
FRAME_SIZE = FRAME_PREFIX.size + 2 * SNAPSHOT_SIZE

assert HEADER.size == 208
assert FRAME_PREFIX.size == 100
assert FRAME_SIZE == 1708


@dataclass(frozen=True)
class ConditionalHeader:
    flags: int
    recording_frames: int
    processed_frames: int
    equal_frames: int
    corrected_frames: int
    pid: int
    library_base: int
    physics_context: int
    car_physics_state: int
    wrapper: int
    native_body: int
    transform_address: int
    linear_address: int
    event_count: int
    read_errors: int
    ptrace_errors: int
    semantic_errors: int
    write_attempts: int
    write_failures: int
    rollback_attempts: int
    rollback_failures: int
    thread_additions: int
    initial_threads: int
    final_threads: int


@dataclass(frozen=True)
class ConditionalFrame:
    tick: int
    monotonic_ns: int
    flags: int
    recorded_transform: bytes
    recorded_linear: bytes
    before: bytes
    immediate: bytes


def _float_components(raw: bytes) -> tuple[float, ...]:
    return tuple(value for (value,) in struct.iter_unpack("<f", raw))


def _finite(raw: bytes) -> bool:
    return all(
        math.isfinite(value) and abs(value) <= 1_000_000.0
        for value in _float_components(raw)
    )


def _component_equal(left: bytes, right: bytes) -> bool:
    return all(
        left_value == right_value
        for left_value, right_value in zip(
            _float_components(left), _float_components(right), strict=True
        )
    )


def read_report(path: Path) -> tuple[ConditionalHeader, tuple[ConditionalFrame, ...]]:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("conditional report is shorter than its header")
    values = HEADER.unpack_from(blob)
    (
        magic,
        version,
        header_size,
        frame_size,
        flags,
        recording_frames,
        processed_frames,
        equal_frames,
        corrected_frames,
        build_id,
        _reserved,
        *tail,
    ) = values
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported A9CDT1 magic/version")
    if header_size != HEADER.size or frame_size != FRAME_SIZE:
        raise ValueError("A9CDT1 ABI size mismatch")
    if build_id != SUPPORTED_BUILD_ID:
        raise ValueError("A9CDT1 build ID mismatch")
    if not 1 <= recording_frames <= 3600:
        raise ValueError("invalid A9CDT1 recording frame count")
    expected_size = HEADER.size + recording_frames * FRAME_SIZE
    if len(blob) != expected_size:
        raise ValueError(
            f"conditional report length must be exactly {expected_size} bytes"
        )
    header = ConditionalHeader(
        flags,
        recording_frames,
        processed_frames,
        equal_frames,
        corrected_frames,
        *tail,
    )
    frames: list[ConditionalFrame] = []
    cursor = HEADER.size
    for _ in range(recording_frames):
        tick, monotonic_ns, frame_flags, _reserved2, transform, linear = (
            FRAME_PREFIX.unpack_from(blob, cursor)
        )
        before_start = cursor + FRAME_PREFIX.size
        immediate_start = before_start + SNAPSHOT_SIZE
        frames.append(
            ConditionalFrame(
                tick,
                monotonic_ns,
                frame_flags,
                transform,
                linear,
                blob[before_start:immediate_start],
                blob[immediate_start : immediate_start + SNAPSHOT_SIZE],
            )
        )
        cursor += FRAME_SIZE
    return header, tuple(frames)


def assess(
    header: ConditionalHeader,
    frames: tuple[ConditionalFrame, ...],
    *,
    minimum_corrected: int = 0,
    maximum_frames: int | None = None,
) -> list[str]:
    problems: list[str] = []
    if minimum_corrected < 0:
        raise ValueError("minimum_corrected must be nonnegative")
    if maximum_frames is not None and maximum_frames < 1:
        raise ValueError("maximum_frames must be positive")
    if header.flags != REQUIRED_HEADER_FLAGS:
        problems.append(
            f"header flags 0x{header.flags:x} != required 0x{REQUIRED_HEADER_FLAGS:x}"
        )
    if header.processed_frames != header.recording_frames or len(frames) != header.recording_frames:
        problems.append("not every recording frame was processed")
    if header.equal_frames + header.corrected_frames != header.recording_frames:
        problems.append("equal/corrected counters do not cover the recording")
    if header.corrected_frames < minimum_corrected:
        problems.append(
            f"corrected frames {header.corrected_frames} < required {minimum_corrected}"
        )
    if maximum_frames is not None and header.recording_frames > maximum_frames:
        problems.append(
            f"recording frames {header.recording_frames} > maximum {maximum_frames}"
        )
    if header.write_attempts != header.corrected_frames or header.write_failures:
        problems.append(
            f"write counters attempts={header.write_attempts} failures={header.write_failures}"
        )
    if header.rollback_attempts or header.rollback_failures:
        problems.append(
            f"rollback counters attempts={header.rollback_attempts} "
            f"failures={header.rollback_failures}"
        )
    if header.read_errors or header.ptrace_errors or header.semantic_errors:
        problems.append(
            "nonzero errors: "
            f"read={header.read_errors} ptrace={header.ptrace_errors} "
            f"semantic={header.semantic_errors}"
        )
    if header.final_threads != header.initial_threads + header.thread_additions:
        problems.append("not every attached thread detached cleanly")
    if header.native_body == 0 or header.wrapper == 0:
        problems.append("invalid wrapper/native identity")
    if header.transform_address != header.native_body + 0x10:
        problems.append("transform address is not native+0x10")
    if header.linear_address != header.native_body + 0x150:
        problems.append("linear address is not native+0x150")

    observed_equal = 0
    observed_corrected = 0
    previous_tick: int | None = None
    previous_time: int | None = None
    for index, frame in enumerate(frames):
        if previous_tick is not None and frame.tick != previous_tick + 1:
            problems.append(f"frame {index}: recording tick is not contiguous")
        if previous_time is not None and frame.monotonic_ns < previous_time:
            problems.append(f"frame {index}: recording time moved backward")
        previous_tick = frame.tick
        previous_time = frame.monotonic_ns
        recorded = frame.recorded_transform + frame.recorded_linear
        if not _finite(recorded):
            problems.append(f"frame {index}: recorded payload is non-finite/out-of-range")
            continue
        current = frame.before[0x10:0x50] + frame.before[0x150:0x15C]
        equal = _component_equal(current, recorded)
        required_frame_flags = (
            FRAME_EQUAL | FRAME_AUDIT_EXACT
            if equal
            else FRAME_CORRECTED | FRAME_AUDIT_EXACT
        )
        if frame.flags != required_frame_flags:
            problems.append(
                f"frame {index}: flags 0x{frame.flags:x} do not match comparator result"
            )
        if equal:
            observed_equal += 1
        else:
            observed_corrected += 1
        expected = bytearray(frame.before)
        if not equal:
            expected[0x10:0x50] = frame.recorded_transform
            expected[0x150:0x15C] = frame.recorded_linear
        if bytes(expected) != frame.immediate:
            first = next(
                byte_index
                for byte_index, (left, right) in enumerate(
                    zip(expected, frame.immediate, strict=True)
                )
                if left != right
            )
            problems.append(
                f"frame {index}: immediate audit differs at snapshot byte {first}"
            )
    if observed_equal != header.equal_frames:
        problems.append("equal frame counter differs from frame audits")
    if observed_corrected != header.corrected_frames:
        problems.append("corrected frame counter differs from frame audits")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--require-supported", action="store_true")
    parser.add_argument("--minimum-corrected", type=int, default=0)
    parser.add_argument("--maximum-frames", type=int)
    args = parser.parse_args()
    if args.minimum_corrected < 0:
        parser.error("--minimum-corrected must be nonnegative")
    if args.maximum_frames is not None and args.maximum_frames < 1:
        parser.error("--maximum-frames must be positive")
    try:
        header, frames = read_report(args.report)
        problems = assess(
            header,
            frames,
            minimum_corrected=args.minimum_corrected,
            maximum_frames=args.maximum_frames,
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"conditional_audit_error={error}")
        return 2
    supported = not problems
    print(
        f"processed={header.processed_frames}/{header.recording_frames} "
        f"equal={header.equal_frames} corrected={header.corrected_frames} "
        f"writes={header.write_attempts} supported={int(supported)}"
    )
    print(
        f"native=0x{header.native_body:x} "
        f"threads={header.initial_threads}+{header.thread_additions}"
        f"->{header.final_threads}"
    )
    for problem in problems:
        print(f"reject={problem}")
    return 0 if supported or not args.require_supported else 1


if __name__ == "__main__":
    raise SystemExit(main())
