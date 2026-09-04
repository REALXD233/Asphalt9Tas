#!/usr/bin/env python3
"""Parse A9HSB1/A9HSB2 passive scheduler-boundary traces."""

from __future__ import annotations

import argparse
import collections
import json
import math
import re
import struct
from pathlib import Path


HEADER = struct.Struct("<8sIIII" + "Q" * 16 + "II16s")
EVENT_V1 = struct.Struct("<QQiIQqIIIIII")
EVENT_V2_CONTEXT = struct.Struct("<" + "Q" * 16 + "II" + "Q" * 32)
EVENT_V2_SIZE = EVENT_V1.size + EVENT_V2_CONTEXT.size
MAGIC_V1 = b"A9HSB1\0\0"
MAGIC_V2 = b"A9HSB2\0\0"
HIT_ACCUMULATOR = 1 << 0
HIT_C98 = 1 << 1
HIT_C9C = 1 << 2
HIT_STATE_WATCH = 1 << 3
HEADER_STATE_WATCH_ENABLED = 1 << 4


def float_from_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def interval_summary(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "min": min(values) if values else None,
        "max": max(values) if values else None,
        "mean": (sum(values) / len(values)) if values else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--events", action="store_true", help="include every decoded event"
    )
    parser.add_argument(
        "--maps", type=Path, help="matching /proc/PID/maps snapshot for stack RVAs"
    )
    args = parser.parse_args()

    mappings: list[tuple[int, int, str]] = []
    if args.maps:
        maps_pattern = re.compile(
            r"^([0-9a-fA-F]+)-([0-9a-fA-F]+)\s+\S+\s+\S+\s+\S+\s+\S+\s*(.*)$"
        )
        for line in args.maps.read_text(encoding="utf-8", errors="replace").splitlines():
            match = maps_pattern.match(line)
            if not match:
                continue
            begin = int(match.group(1), 16)
            end = int(match.group(2), 16)
            label = match.group(3).strip() or "[anonymous]"
            mappings.append((begin, end, label))

    def mapped_word(value: int) -> str | None:
        for begin, end, label in mappings:
            if begin <= value < end:
                return f"{label}+0x{value - begin:x}"
        return None

    data = args.trace.read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit("trace is shorter than its header")
    raw = HEADER.unpack_from(data)
    (
        magic,
        version,
        header_size,
        event_size,
        flags,
        pid,
        library_base,
        main_object,
        accumulator_address,
        final_owner,
        c98_address,
        c9c_address,
        start_ns,
        event_count,
        accumulator_hits,
        c98_hits,
        c9c_hits,
        read_errors,
        ptrace_errors,
        thread_additions,
        unexpected_stops,
        initial_threads,
        final_threads,
        reserved,
    ) = raw
    if (magic, version) not in ((MAGIC_V1, 1), (MAGIC_V2, 2)):
        raise SystemExit(f"unsupported trace magic/version: {magic!r}/{version}")
    expected_event_size = EVENT_V1.size if version == 1 else EVENT_V2_SIZE
    if header_size != HEADER.size or event_size != expected_event_size:
        raise SystemExit(
            f"ABI mismatch: header={header_size}/{HEADER.size} "
            f"event={event_size}/{expected_event_size}"
        )
    expected_size = header_size + event_count * event_size
    if len(data) != expected_size:
        raise SystemExit(f"size mismatch: file={len(data)} expected={expected_size}")
    state_watch_address, state_watch_hits = struct.unpack("<QQ", reserved)
    state_watch_enabled = bool(flags & HEADER_STATE_WATCH_ENABLED)

    decoded: list[dict[str, object]] = []
    ordered_events: list[dict[str, object]] = []
    accumulator_values: collections.Counter[int] = collections.Counter()
    accumulator_hit_values: collections.Counter[int] = collections.Counter()
    enabled_values: collections.Counter[int] = collections.Counter()
    time_scale_values: collections.Counter[float] = collections.Counter()
    c98_values: collections.Counter[float] = collections.Counter()
    c9c_values: collections.Counter[float] = collections.Counter()
    tids: collections.Counter[int] = collections.Counter()
    rips: collections.Counter[str] = collections.Counter()
    accumulator_times: list[int] = []
    state_watch_values: collections.Counter[float] = collections.Counter()
    state_watch_rips: collections.Counter[str] = collections.Counter()
    state_watch_stack_words: collections.Counter[str] = collections.Counter()
    state_watch_stack_mappings: collections.Counter[str] = collections.Counter()
    state_watch_stack_read_errors = 0
    gpr_names = (
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
        "r10", "r11", "r12", "r13", "r14", "r15",
    )
    for index in range(event_count):
        offset = header_size + index * event_size
        (
            sequence,
            monotonic_ns,
            tid,
            event_flags,
            rip,
            accumulator,
            enabled,
            time_scale_bits,
            c98_bits,
            c9c_bits,
            read_ok,
            state_watch_bits,
        ) = EVENT_V1.unpack_from(data, offset)
        rsp = rbp = 0
        gprs: tuple[int, ...] = ()
        stack_read_ok = 0
        stack_words: tuple[int, ...] = ()
        if version == 2:
            context = EVENT_V2_CONTEXT.unpack_from(data, offset + EVENT_V1.size)
            rsp, rbp = context[:2]
            gprs = context[2:16]
            stack_read_ok = context[16]
            stack_words = context[18:]
        scale = float_from_bits(time_scale_bits)
        c98 = float_from_bits(c98_bits)
        c9c = float_from_bits(c9c_bits)
        state_watch = float_from_bits(state_watch_bits)
        tids[tid] += 1
        rips[f"0x{rip:x}"] += 1
        if read_ok:
            accumulator_values[accumulator] += 1
            enabled_values[enabled] += 1
            if math.isfinite(scale):
                time_scale_values[round(scale, 7)] += 1
            if math.isfinite(c98):
                c98_values[round(c98, 7)] += 1
            if math.isfinite(c9c):
                c9c_values[round(c9c, 7)] += 1
        if event_flags & HIT_ACCUMULATOR:
            accumulator_times.append(monotonic_ns)
            accumulator_hit_values[accumulator] += 1
        kinds: list[str] = []
        if event_flags & HIT_ACCUMULATOR:
            kinds.append("ACC_ZERO" if accumulator == 0 else "ACC_NONZERO")
        if event_flags & HIT_C98:
            kinds.append("C98")
        if event_flags & HIT_C9C:
            kinds.append("C9C")
        if event_flags & HIT_STATE_WATCH:
            kinds.append("STATE_WRITE")
            state_watch_rips[f"0x{rip:x}"] += 1
            if math.isfinite(state_watch):
                state_watch_values[round(state_watch, 7)] += 1
            if version == 2:
                if stack_read_ok:
                    for word in stack_words:
                        if word:
                            state_watch_stack_words[f"0x{word:x}"] += 1
                            mapped = mapped_word(word)
                            if mapped:
                                state_watch_stack_mappings[mapped] += 1
                else:
                    state_watch_stack_read_errors += 1
        event = {
            "sequence": sequence,
            "monotonic_ns": monotonic_ns,
            "relative_ms": (monotonic_ns - start_ns) / 1_000_000.0,
            "tid": tid,
            "flags": event_flags,
            "kind": "+".join(kinds) if kinds else "UNKNOWN",
            "rip": f"0x{rip:x}",
            "accumulator": accumulator,
            "enabled": enabled,
            "time_scale": scale if math.isfinite(scale) else None,
            "c98": c98 if math.isfinite(c98) else None,
            "c9c": c9c if math.isfinite(c9c) else None,
            "state_watch": (
                state_watch if state_watch_enabled and math.isfinite(state_watch)
                else None
            ),
            "read_ok": bool(read_ok),
        }
        if version == 2 and event_flags & HIT_STATE_WATCH:
            event["rsp"] = f"0x{rsp:x}"
            event["rbp"] = f"0x{rbp:x}"
            event["gprs"] = {
                name: f"0x{value:x}" for name, value in zip(gpr_names, gprs)
            }
            event["stack_read_ok"] = bool(stack_read_ok)
            event["stack_words"] = [f"0x{word:x}" for word in stack_words]
        ordered_events.append(event)
        if args.events:
            decoded.append({k: v for k, v in event.items() if k != "monotonic_ns"})

    intervals_ms = [
        (right - left) / 1_000_000.0
        for left, right in zip(accumulator_times, accumulator_times[1:])
    ]
    accumulator_events = [
        event for event in ordered_events if event["flags"] & HIT_ACCUMULATOR
    ]
    accumulator_transitions: collections.Counter[str] = collections.Counter()
    for left, right in zip(accumulator_events, accumulator_events[1:]):
        accumulator_transitions[f"{left['kind']}->{right['kind']}"] += 1

    nonzero_to_zero_ms: list[float] = []
    zero_to_nonzero_ms: list[float] = []
    for left, right in zip(accumulator_events, accumulator_events[1:]):
        elapsed = (right["monotonic_ns"] - left["monotonic_ns"]) / 1_000_000.0
        if left["kind"] == "ACC_NONZERO" and right["kind"] == "ACC_ZERO":
            nonzero_to_zero_ms.append(elapsed)
        elif left["kind"] == "ACC_ZERO" and right["kind"] == "ACC_NONZERO":
            zero_to_nonzero_ms.append(elapsed)

    cycle_signatures: collections.Counter[str] = collections.Counter()
    cycle_control_counts: collections.Counter[str] = collections.Counter()
    cycle_state_counts: collections.Counter[str] = collections.Counter()
    state_order_counts: collections.Counter[str] = collections.Counter()
    c9c_to_state_ms: list[float] = []
    state_to_post_ms: list[float] = []
    complete_cycles = 0
    active_cycles = 0
    cycle_start: int | None = None
    for index, event in enumerate(ordered_events):
        if event["kind"] == "ACC_NONZERO":
            cycle_start = index
        elif event["kind"] == "ACC_ZERO" and cycle_start is not None:
            window = ordered_events[cycle_start : index + 1]
            signature = ">".join(str(item["kind"]) for item in window)
            c98_count = sum(bool(item["flags"] & HIT_C98) for item in window)
            c9c_count = sum(bool(item["flags"] & HIT_C9C) for item in window)
            state_count = sum(
                bool(item["flags"] & HIT_STATE_WATCH) for item in window
            )
            cycle_signatures[signature] += 1
            cycle_control_counts[f"c98={c98_count},c9c={c9c_count}"] += 1
            cycle_state_counts[f"state={state_count}"] += 1
            c9c_events = [item for item in window if item["flags"] & HIT_C9C]
            state_events = [
                item for item in window if item["flags"] & HIT_STATE_WATCH
            ]
            if len(c9c_events) == 1 and len(state_events) == 1:
                c9c_event = c9c_events[0]
                state_event = state_events[0]
                order = (
                    "C9C>STATE"
                    if c9c_event["monotonic_ns"] < state_event["monotonic_ns"]
                    else "STATE>C9C"
                )
                state_order_counts[order] += 1
                if order == "C9C>STATE":
                    c9c_to_state_ms.append(
                        (state_event["monotonic_ns"] - c9c_event["monotonic_ns"])
                        / 1_000_000.0
                    )
                    state_to_post_ms.append(
                        (window[-1]["monotonic_ns"] - state_event["monotonic_ns"])
                        / 1_000_000.0
                    )
            complete_cycles += 1
            active_cycles += int(c98_count > 0 or c9c_count > 0)
            cycle_start = None

    control_events = [
        event
        for event in ordered_events
        if event["flags"] & (HIT_C98 | HIT_C9C)
    ]
    rip_kind_histogram: collections.Counter[str] = collections.Counter()
    for event in ordered_events:
        rip_kind_histogram[f"{event['rip']}:{event['kind']}"] += 1

    nonzero_microseconds = [
        float(event["accumulator"])
        for event in accumulator_events
        if event["kind"] == "ACC_NONZERO"
    ]
    summary: dict[str, object] = {
        "trace": str(args.trace),
        "version": version,
        "clean": bool(flags & 1),
        "target_signature_verified": bool(flags & (1 << 2)),
        "pid": pid,
        "library_base": f"0x{library_base:x}",
        "main_object": f"0x{main_object:x}",
        "accumulator_address": f"0x{accumulator_address:x}",
        "final_owner": f"0x{final_owner:x}",
        "c98_address": f"0x{c98_address:x}",
        "c9c_address": f"0x{c9c_address:x}",
        "event_count": event_count,
        "accumulator_hits": accumulator_hits,
        "c98_hits": c98_hits,
        "c9c_hits": c9c_hits,
        "state_watch_enabled": state_watch_enabled,
        "state_watch_address": (
            f"0x{state_watch_address:x}" if state_watch_enabled else None
        ),
        "state_watch_hits": state_watch_hits if state_watch_enabled else 0,
        "state_watch_rip_histogram": dict(state_watch_rips.most_common()),
        "state_watch_values": {
            str(value): count for value, count in state_watch_values.most_common(20)
        },
        "state_watch_stack_read_errors": state_watch_stack_read_errors,
        "state_watch_stack_words_top100": dict(
            state_watch_stack_words.most_common(100)
        ),
        "state_watch_stack_mappings_top100": dict(
            state_watch_stack_mappings.most_common(100)
        ),
        "read_errors": read_errors,
        "ptrace_errors": ptrace_errors,
        "unexpected_stops": unexpected_stops,
        "initial_threads": initial_threads,
        "final_threads": final_threads,
        "thread_additions": thread_additions,
        "tid_histogram": dict(tids.most_common()),
        "rip_histogram": dict(rips.most_common()),
        "rip_kind_histogram": dict(rip_kind_histogram.most_common()),
        "accumulator_sample_values_top20": {
            str(value): count
            for value, count in accumulator_values.most_common(20)
        },
        "accumulator_hit_values_top20": {
            str(value): count
            for value, count in accumulator_hit_values.most_common(20)
        },
        "accumulator_nonzero_microseconds": interval_summary(nonzero_microseconds),
        "enabled_values": {
            str(value): count for value, count in enabled_values.most_common()
        },
        "time_scale_values": {
            str(value): count for value, count in time_scale_values.most_common()
        },
        "c98_values": {
            str(value): count for value, count in c98_values.most_common()
        },
        "c9c_values": {
            str(value): count for value, count in c9c_values.most_common()
        },
        "accumulator_interval_ms": interval_summary(intervals_ms),
        "accumulator_phase_interval_ms": {
            "nonzero_to_zero": interval_summary(nonzero_to_zero_ms),
            "zero_to_nonzero": interval_summary(zero_to_nonzero_ms),
        },
        "accumulator_transitions": dict(accumulator_transitions.most_common()),
        "cycle_analysis": {
            "complete_cycles": complete_cycles,
            "active_cycles": active_cycles,
            "inactive_cycles": complete_cycles - active_cycles,
            "control_counts": dict(cycle_control_counts.most_common()),
            "state_counts": dict(cycle_state_counts.most_common()),
            "state_order": dict(state_order_counts.most_common()),
            "c9c_to_state_ms": interval_summary(c9c_to_state_ms),
            "state_to_post_ms": interval_summary(state_to_post_ms),
            "signatures": dict(cycle_signatures.most_common()),
        },
        "control_window_ms": {
            "first": control_events[0]["relative_ms"] if control_events else None,
            "last": control_events[-1]["relative_ms"] if control_events else None,
            "span": (
                control_events[-1]["relative_ms"] - control_events[0]["relative_ms"]
                if control_events
                else None
            ),
        },
    }
    if args.events:
        summary["events"] = decoded
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
