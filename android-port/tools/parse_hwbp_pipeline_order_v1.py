#!/usr/bin/env python3
"""Fail-closed parser for the host-only physics pipeline-order trace."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"A9PIP1\0\0"
VERSION = 1
CLEAN = 1 << 0
TARGET_VERIFIED = 1 << 1
HIT_COMPLETION = 1 << 0
HIT_CALLBACK_FLAGS = 1 << 1
HIT_F64 = 1 << 2
HIT_ACCUMULATOR = 1 << 3
HEADER = struct.Struct("<8sIIII20QII")
EVENT = struct.Struct("<QQiIQQHHIIIII")


@dataclass(frozen=True)
class Header:
    flags: int
    pid: int
    library_base: int
    physics_context: int
    completion_address: int
    callback_flags_address: int
    car_physics_state: int
    f64_address: int
    backend_adapter: int
    inner_world: int
    accumulator_address: int
    start_ns: int
    event_count: int
    completion_hits: int
    callback_flag_hits: int
    f64_hits: int
    accumulator_hits: int
    read_errors: int
    ptrace_errors: int
    thread_additions: int
    unexpected_stops: int
    initial_threads: int
    final_threads: int


@dataclass(frozen=True)
class EventRecord:
    sequence: int
    monotonic_ns: int
    tid: int
    flags: int
    rip: int
    completion_token: int
    callback_flags: int
    f64_bits: int
    accumulator_bits: int
    read_ok: int


def bits_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def read_trace(path: Path) -> tuple[Header, list[EventRecord]]:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("trace is shorter than its header")
    values = HEADER.unpack_from(blob)
    magic, version, header_size, event_size, flags = values[:5]
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported pipeline trace magic/version")
    if header_size != HEADER.size or event_size != EVENT.size:
        raise ValueError("pipeline trace ABI size mismatch")
    header = Header(flags, *values[5:])
    expected_size = HEADER.size + header.event_count * EVENT.size
    if len(blob) != expected_size:
        raise ValueError(
            f"trace size mismatch: expected {expected_size}, got {len(blob)}"
        )
    events: list[EventRecord] = []
    for index in range(header.event_count):
        row = EVENT.unpack_from(blob, HEADER.size + index * EVENT.size)
        events.append(
            EventRecord(
                sequence=row[0],
                monotonic_ns=row[1],
                tid=row[2],
                flags=row[3],
                rip=row[4],
                completion_token=row[5],
                callback_flags=row[6],
                f64_bits=row[8],
                accumulator_bits=row[9],
                read_ok=row[10],
            )
        )
    return header, events


def assess(header: Header, events: list[EventRecord], minimum_cycles: int) -> tuple[bool, list[str], dict[str, int]]:
    problems: list[str] = []
    if not (header.flags & TARGET_VERIFIED):
        problems.append("target build/object chain was not verified")
    if not (header.flags & CLEAN):
        problems.append("transport did not finish cleanly")
    if header.read_errors or header.ptrace_errors or header.unexpected_stops:
        problems.append(
            "nonzero transport errors: "
            f"read={header.read_errors} ptrace={header.ptrace_errors} "
            f"unexpected={header.unexpected_stops}"
        )
    if header.final_threads != header.initial_threads + header.thread_additions:
        problems.append("not every attached thread detached cleanly")
    if any(event.sequence != index for index, event in enumerate(events)):
        problems.append("event sequence is not contiguous")
    if any(events[index].monotonic_ns > events[index + 1].monotonic_ns for index in range(len(events) - 1)):
        problems.append("event timestamps are not monotonic")
    if any(not event.read_ok for event in events):
        problems.append("one or more events lack a complete value snapshot")

    observed_counts = {
        "completion": sum(bool(event.flags & HIT_COMPLETION) for event in events),
        "callback_flags": sum(bool(event.flags & HIT_CALLBACK_FLAGS) for event in events),
        "f64": sum(bool(event.flags & HIT_F64) for event in events),
        "accumulator": sum(bool(event.flags & HIT_ACCUMULATOR) for event in events),
    }
    expected_counts = {
        "completion": header.completion_hits,
        "callback_flags": header.callback_flag_hits,
        "f64": header.f64_hits,
        "accumulator": header.accumulator_hits,
    }
    if observed_counts != expected_counts:
        problems.append(
            f"header/event hit counters differ: header={expected_counts} "
            f"events={observed_counts}"
        )

    state = "waiting"
    cycles = 0
    ignored_prefix = 0
    incomplete_tail = 0
    owner_tid = None
    order_errors: list[str] = []
    for event in events:
        event_types = []
        if event.flags & HIT_COMPLETION:
            event_types.append("completion")
        if event.flags & HIT_CALLBACK_FLAGS:
            event_types.append("callback")
        if event.flags & HIT_F64:
            event_types.append("f64")
        if event.flags & HIT_ACCUMULATOR:
            event_types.append("accumulator")
        for event_type in event_types:
            if state == "waiting":
                if event_type == "completion":
                    state = "completed"
                    owner_tid = event.tid
                else:
                    ignored_prefix += 1
                continue
            if event_type == "completion":
                order_errors.append(
                    f"seq {event.sequence}: new completion before prior cycle reached next accumulator"
                )
                state = "completed"
                owner_tid = event.tid
            elif event_type == "callback":
                dispatching = event.callback_flags & 0xFF
                if state == "completed" and dispatching == 1:
                    if event.tid != owner_tid:
                        order_errors.append(
                            f"seq {event.sequence}: callback opened on tid {event.tid}, expected {owner_tid}"
                        )
                    state = "callback_open"
                elif state == "published" and dispatching == 0:
                    if event.tid != owner_tid:
                        order_errors.append(
                            f"seq {event.sequence}: callback closed on tid {event.tid}, expected {owner_tid}"
                        )
                    state = "callback_closed"
                elif state == "callback_closed" and dispatching == 0:
                    # The later +0x1A1 deferred-flag clear is expected to hit
                    # the same 2-byte watch after the dispatching-byte clear.
                    pass
                else:
                    order_errors.append(
                        f"seq {event.sequence}: callback flags=0x{event.callback_flags:04x} in state {state}"
                    )
            elif event_type == "f64":
                value = bits_float(event.f64_bits)
                if not math.isfinite(value) or abs(value) > 1_000_000.0:
                    order_errors.append(
                        f"seq {event.sequence}: non-finite/out-of-range F64 value"
                    )
                if state != "callback_open":
                    order_errors.append(
                        f"seq {event.sequence}: F64 publication in state {state}"
                    )
                else:
                    if event.tid != owner_tid:
                        order_errors.append(
                            f"seq {event.sequence}: F64 tid {event.tid}, expected owner {owner_tid}"
                        )
                    state = "published"
            elif event_type == "accumulator":
                value = bits_float(event.accumulator_bits)
                if not math.isfinite(value) or abs(value) > 60.0:
                    order_errors.append(
                        f"seq {event.sequence}: non-finite/out-of-range accumulator"
                    )
                if state != "callback_closed":
                    order_errors.append(
                        f"seq {event.sequence}: next accumulator write in state {state}"
                    )
                else:
                    cycles += 1
                    state = "waiting"
                    owner_tid = None
    if state != "waiting":
        incomplete_tail = 1
    if order_errors:
        problems.extend(order_errors[:20])
        if len(order_errors) > 20:
            problems.append(f"{len(order_errors) - 20} additional order errors omitted")
    if cycles < minimum_cycles:
        problems.append(f"complete ordered cycles {cycles} < required {minimum_cycles}")

    stats = {
        "cycles": cycles,
        "ignored_prefix_events": ignored_prefix,
        "incomplete_tail": incomplete_tail,
        **observed_counts,
    }
    return not problems, problems, stats


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--minimum-cycles", type=int, default=20)
    parser.add_argument("--require-supported", action="store_true")
    args = parser.parse_args()
    if args.minimum_cycles < 1:
        parser.error("--minimum-cycles must be positive")
    try:
        header, events = read_trace(args.trace)
        supported, problems, stats = assess(header, events, args.minimum_cycles)
    except (OSError, ValueError, struct.error) as error:
        print(f"pipeline_trace_error={error}")
        return 2
    print(
        f"events={len(events)} completion={stats['completion']} "
        f"callback_flags={stats['callback_flags']} f64={stats['f64']} "
        f"accumulator={stats['accumulator']} cycles={stats['cycles']} "
        f"ignored_prefix={stats['ignored_prefix_events']} "
        f"incomplete_tail={stats['incomplete_tail']}"
    )
    print(
        f"context=0x{header.physics_context:x} car_state=0x{header.car_physics_state:x} "
        f"world=0x{header.inner_world:x} transport_clean={int(bool(header.flags & CLEAN))} "
        f"supported={int(supported)}"
    )
    for problem in problems:
        print(f"reject={problem}")
    return 0 if supported or not args.require_supported else 1


if __name__ == "__main__":
    raise SystemExit(main())
