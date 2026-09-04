#!/usr/bin/env python3
"""Validate an A9AFD1 five-frame authoritative fixed-delta report."""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import struct
import sys


MAGIC = b"A9AFD1\0\0"
VERSION = 1
FRAME_COUNT = 5
REQUIRED_FLAGS = 0x7F
HEADER = struct.Struct("<8sIIII20sI" + "Q" * 21 + "I" * 4)
FRAME = struct.Struct("<IIiiqq" + "Q" * 10 + "HHI")

if HEADER.size != 232:
    raise RuntimeError(f"internal header ABI mismatch: {HEADER.size}")
if FRAME.size != 120:
    raise RuntimeError(f"internal frame ABI mismatch: {FRAME.size}")


class ValidationError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class ValidationResult:
    path: str
    size: int
    pid: int
    fixed_interval_us: int
    bound_frames: int
    event_count: int
    target_memory_write_attempts: int
    delta_writes: int
    gameplay_action_calls: int
    clean_detach: bool


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def validate_bytes(data: bytes, path: str = "<memory>") -> ValidationResult:
    require(len(data) == HEADER.size + FRAME_COUNT * FRAME.size,
            f"size={len(data)} expected={HEADER.size + FRAME_COUNT * FRAME.size}")
    values = HEADER.unpack_from(data)
    (
        magic, version, header_size, frame_size, flags, build_id, reserved0,
        pid, library_base, main_object, final_owner, physics_context,
        delta_address, c98_address, c9c_address, completion_address,
        callback_flags_address, f64_address, world_accumulator_address,
        event_count, read_errors, ptrace_errors, semantic_errors,
        target_memory_write_attempts, delta_writes, delta_write_failures,
        gameplay_action_calls, thread_additions, initial_threads, final_threads,
        fixed_interval_us, bound_frames,
    ) = values
    require(magic == MAGIC and version == VERSION, "magic/version mismatch")
    require(header_size == HEADER.size and frame_size == FRAME.size,
            "report ABI size mismatch")
    require(flags == REQUIRED_FLAGS, f"flags=0x{flags:x}")
    require(any(build_id) and reserved0 == 0, "invalid build binding")
    require(pid > 0 and library_base > 0, "invalid target identity")
    require(delta_address == main_object + 0x150, "delta address mismatch")
    require(c98_address == final_owner + 0xC98, "C98 address mismatch")
    require(c9c_address == c98_address + 4, "C9C address mismatch")
    require(completion_address == physics_context + 0x1D0,
            "completion address mismatch")
    require(callback_flags_address == physics_context + 0x1A0,
            "callback flags address mismatch")
    require(f64_address > 0 and world_accumulator_address > 0,
            "post-phase address missing")
    require(event_count > 0, "event_count=0")
    require(read_errors == ptrace_errors == semantic_errors == 0,
            "observer error counter is nonzero")
    require(target_memory_write_attempts == FRAME_COUNT,
            "target write attempts are not exactly five")
    require(delta_writes == FRAME_COUNT and delta_write_failures == 0,
            "fixed-delta verification count mismatch")
    require(gameplay_action_calls == 0, "gameplay action call recorded")
    require(final_threads == initial_threads + thread_additions,
            "thread accounting mismatch")
    require(1000 <= fixed_interval_us <= 100000,
            f"fixed_interval_us={fixed_interval_us}")
    require(bound_frames == FRAME_COUNT, f"bound_frames={bound_frames}")

    previous_event = 0
    for index in range(FRAME_COUNT):
        frame = FRAME.unpack_from(data, HEADER.size + index * FRAME.size)
        (
            selected_tick, published_tick, cycle_tid, commit_tid,
            observed_delta_us, applied_delta_us, delta_event, c98_event,
            c9c_event, prefix_event, f64_event, callback_close_event,
            deferred_clear_event, world_commit_event, completion_before,
            completion_after, callback_flags, reserved16, phase_action_or,
        ) = frame
        require(selected_tick == published_tick == index,
                f"frame {index}: tick binding mismatch")
        require(cycle_tid > 0 and commit_tid > 0 and cycle_tid != commit_tid,
                f"frame {index}: invalid thread roles")
        require(0 < observed_delta_us <= 1000000,
                f"frame {index}: invalid observed delta")
        require(applied_delta_us == fixed_interval_us,
                f"frame {index}: applied delta mismatch")
        require(previous_event < delta_event < c98_event < c9c_event,
                f"frame {index}: invalid input event order")
        require(c9c_event == prefix_event,
                f"frame {index}: prefix not certified at C9C")
        require(prefix_event < f64_event < callback_close_event <
                deferred_clear_event < world_commit_event,
                f"frame {index}: invalid post event order")
        require(completion_before != completion_after,
                f"frame {index}: completion token unchanged")
        require((callback_flags & 0xFF) == 1 and reserved16 == 0,
                f"frame {index}: callback/reserved mismatch")
        expected_actions = 333 if index == FRAME_COUNT - 1 else 205
        require(phase_action_or == expected_actions,
                f"frame {index}: action mask mismatch")
        previous_event = world_commit_event
    require(previous_event <= event_count, "frame events exceed event_count")
    return ValidationResult(
        path=path,
        size=len(data),
        pid=pid,
        fixed_interval_us=fixed_interval_us,
        bound_frames=bound_frames,
        event_count=event_count,
        target_memory_write_attempts=target_memory_write_attempts,
        delta_writes=delta_writes,
        gameplay_action_calls=gameplay_action_calls,
        clean_detach=True,
    )


def make_selftest_report() -> bytes:
    fixed = 16667
    addresses = (
        0x100000, 0x200000, 0x300000, 0x100150, 0x200C98,
        0x200C9C, 0x3001D0, 0x3001A0, 0x400F64, 0x500188,
    )
    frames = bytearray()
    event = 0
    for index in range(FRAME_COUNT):
        delta, c98, c9c = event + 1, event + 2, event + 3
        f64, close, deferred, commit = event + 4, event + 5, event + 6, event + 7
        frames += FRAME.pack(
            index, index, 100 + index, 200 + index, 17000, fixed,
            delta, c98, c9c, c9c, f64, close, deferred, commit,
            index, index + 1, 1, 0,
            333 if index == FRAME_COUNT - 1 else 205,
        )
        event = commit
    header = HEADER.pack(
        MAGIC, VERSION, HEADER.size, FRAME.size, REQUIRED_FLAGS,
        bytes(range(1, 21)), 0, 1234, 0x10000000, *addresses,
        event, 0, 0, 0, FRAME_COUNT, FRAME_COUNT, 0, 0, 2,
        10, 12, fixed, FRAME_COUNT,
    )
    return header + bytes(frames)


def selftest() -> None:
    good = make_selftest_report()
    result = validate_bytes(good)
    require(result.delta_writes == FRAME_COUNT, "valid selftest failed")
    bad = bytearray(good)
    # Header Q index 17 is delta_writes: byte 184.
    struct.pack_into("<Q", bad, 184, 4)
    try:
        validate_bytes(bytes(bad))
    except ValidationError:
        pass
    else:
        raise ValidationError("selftest accepted four delta writes")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("--json-out", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        selftest()
        print("A9AFD1_VALIDATOR_SELFTEST passed=1")
        return 0
    if args.report is None:
        parser.error("report is required unless --selftest is used")
    result = validate_bytes(args.report.read_bytes(), str(args.report))
    text = json.dumps(dataclasses.asdict(result), ensure_ascii=False, indent=2) + "\n"
    if args.json_out:
        args.json_out.write_text(text, encoding="utf-8")
    print(
        "A9AFD1_VALID passed=1 frames=5 delta_writes=5 "
        "gameplay_action_calls=0 clean_detach=1"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValidationError, struct.error) as error:
        print(f"A9AFD1_VALID passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
