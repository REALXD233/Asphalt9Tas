#!/usr/bin/env python3
"""Summarize A9HSB2 state-watch register/stack context by host RIP."""

from __future__ import annotations

import argparse
import collections
import json
import re
import statistics
import struct
from pathlib import Path


HEADER = struct.Struct("<8sIIII" + "Q" * 16 + "II16s")
EVENT_PREFIX = struct.Struct("<QQiIQqIIIIII")
CONTEXT = struct.Struct("<" + "Q" * 16 + "II" + "Q" * 32)
MAGIC = b"A9HSB2\0\0"
HIT_STATE = 1 << 3
HIT_ACCUMULATOR = 1 << 0
HIT_C98 = 1 << 1
HIT_C9C = 1 << 2
GPR_NAMES = (
    "rsp", "rbp", "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
)


def interval_summary(times: list[int]) -> dict[str, float | int | None]:
    values = [
        (right - left) / 1_000_000.0 for left, right in zip(times, times[1:])
    ]
    return {
        "count": len(values),
        "min_ms": min(values) if values else None,
        "median_ms": statistics.median(values) if values else None,
        "mean_ms": statistics.fmean(values) if values else None,
        "max_ms": max(values) if values else None,
    }


def value_summary(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "min_ms": min(values) if values else None,
        "median_ms": statistics.median(values) if values else None,
        "mean_ms": statistics.fmean(values) if values else None,
        "max_ms": max(values) if values else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--target", type=lambda value: int(value, 0), required=True)
    parser.add_argument(
        "--marker",
        action="append",
        default=[],
        type=lambda value: int(value, 0),
        help="report zero-based stack-word indices for this exact value",
    )
    args = parser.parse_args()

    mappings: list[tuple[int, int, str]] = []
    pattern = re.compile(
        r"^([0-9a-fA-F]+)-([0-9a-fA-F]+)\s+\S+\s+\S+\s+\S+\s+\S+\s*(.*)$"
    )
    for line in args.maps.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            mappings.append(
                (int(match.group(1), 16), int(match.group(2), 16),
                 match.group(3).strip() or "[anonymous]")
            )

    def map_value(value: int) -> str | None:
        for begin, end, label in mappings:
            if begin <= value < end:
                return f"{label}+0x{value - begin:x}"
        return None

    data = args.trace.read_bytes()
    header = HEADER.unpack_from(data)
    magic, version, header_size, event_size = header[:4]
    event_count = header[13]
    if magic != MAGIC or version != 2:
        raise SystemExit(f"expected A9HSB2, got {magic!r}/{version}")
    if header_size != HEADER.size or event_size != EVENT_PREFIX.size + CONTEXT.size:
        raise SystemExit("trace ABI mismatch")

    per_rip: dict[int, dict[str, object]] = {}
    ordered_state_rips: list[int] = []
    ordered_events: list[tuple[int, int, int, int]] = []
    for index in range(event_count):
        offset = header_size + index * event_size
        prefix = EVENT_PREFIX.unpack_from(data, offset)
        monotonic_ns, tid, flags, rip = prefix[1], prefix[2], prefix[3], prefix[4]
        accumulator = prefix[5]
        ordered_events.append((monotonic_ns, flags, rip, accumulator))
        if not flags & HIT_STATE:
            continue
        context = CONTEXT.unpack_from(data, offset + EVENT_PREFIX.size)
        registers = context[:16]
        stack_ok = context[16]
        stack = context[18:]
        bucket = per_rip.setdefault(
            rip,
            {
                "count": 0,
                "tids": collections.Counter(),
                "times": [],
                "target_registers": collections.Counter(),
                "mapped_stack": collections.Counter(),
                "marker_indices": {
                    marker: collections.Counter() for marker in args.marker
                },
                "stack_errors": 0,
            },
        )
        bucket["count"] += 1
        bucket["tids"][tid] += 1
        bucket["times"].append(monotonic_ns)
        for name, value in zip(GPR_NAMES, registers):
            if value == args.target:
                bucket["target_registers"][name] += 1
        if not stack_ok:
            bucket["stack_errors"] += 1
        else:
            for stack_index, value in enumerate(stack):
                mapped = map_value(value)
                if mapped:
                    bucket["mapped_stack"][mapped] += 1
                for marker in args.marker:
                    if value == marker:
                        bucket["marker_indices"][marker][stack_index] += 1
        ordered_state_rips.append(rip)

    transitions = collections.Counter(
        f"0x{left:x}->0x{right:x}"
        for left, right in zip(ordered_state_rips, ordered_state_rips[1:])
    )
    cycle_presence: dict[int, collections.Counter[int]] = collections.defaultdict(
        collections.Counter
    )
    cycle_last_to_zero_ms: dict[int, list[float]] = collections.defaultdict(list)
    cycle_order: dict[int, collections.Counter[str]] = collections.defaultdict(
        collections.Counter
    )
    complete_cycles = 0
    cycle_start: int | None = None
    for index, (_, flags, _, accumulator) in enumerate(ordered_events):
        if flags & HIT_ACCUMULATOR and accumulator != 0:
            cycle_start = index
        elif flags & HIT_ACCUMULATOR and accumulator == 0 and cycle_start is not None:
            cycle = ordered_events[cycle_start:index + 1]
            state_times: dict[int, list[int]] = collections.defaultdict(list)
            for event_time, event_flags, event_rip, _ in cycle:
                if event_flags & HIT_STATE:
                    state_times[event_rip].append(event_time)
            state_counts = collections.Counter(
                {state_rip: len(times) for state_rip, times in state_times.items()}
            )
            c98_times = [time for time, event_flags, _, _ in cycle if event_flags & HIT_C98]
            c9c_times = [time for time, event_flags, _, _ in cycle if event_flags & HIT_C9C]
            zero_time = ordered_events[index][0]
            for state_rip, count in state_counts.items():
                cycle_presence[state_rip][count] += 1
                last_time = state_times[state_rip][-1]
                cycle_last_to_zero_ms[state_rip].append(
                    (zero_time - last_time) / 1_000_000.0
                )
                for label, control_times in (("c98", c98_times), ("c9c", c9c_times)):
                    if not control_times:
                        cycle_order[state_rip][f"no_{label}"] += 1
                    elif last_time < control_times[0]:
                        cycle_order[state_rip][f"last_before_{label}"] += 1
                    else:
                        cycle_order[state_rip][f"last_after_{label}"] += 1
            complete_cycles += 1
            cycle_start = None
    output: dict[str, object] = {
        "trace": str(args.trace),
        "target": f"0x{args.target:x}",
        "state_events": len(ordered_state_rips),
        "complete_accumulator_cycles": complete_cycles,
        "transitions": dict(transitions.most_common(20)),
        "by_rip": {},
    }
    for rip, bucket in sorted(per_rip.items(), key=lambda item: -item[1]["count"]):
        output["by_rip"][f"0x{rip:x}"] = {
            "count": bucket["count"],
            "tids": dict(bucket["tids"].most_common()),
            "intervals": interval_summary(bucket["times"]),
            "target_registers": dict(bucket["target_registers"].most_common()),
            "stack_errors": bucket["stack_errors"],
            "mapped_stack_top30": dict(bucket["mapped_stack"].most_common(30)),
            "marker_stack_indices": {
                f"0x{marker:x}": dict(indices.most_common())
                for marker, indices in bucket["marker_indices"].items()
            },
            "hits_per_accumulator_cycle": dict(
                sorted(cycle_presence[rip].items())
            ),
            "cycles_present": sum(cycle_presence[rip].values()),
            "last_hit_to_accumulator_zero": value_summary(
                cycle_last_to_zero_ms[rip]
            ),
            "cycle_control_order": dict(cycle_order[rip].most_common()),
        }
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
