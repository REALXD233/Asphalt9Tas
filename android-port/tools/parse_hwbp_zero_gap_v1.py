#!/usr/bin/env python3
"""Parse and assess an A9ZGB1 low-overhead zero-gap trace."""

from __future__ import annotations

import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any


HEADER = struct.Struct("<8sIIII" + "Q" * 18 + "II")
EVENT = struct.Struct("<QQiIQqIIII")
MAGIC = b"A9ZGB1\0\0"
VERSION = 1

HEADER_CLEAN = 1 << 0
HEADER_TARGET_VERIFIED = 1 << 1
HEADER_ROTATION_WATCH = 1 << 2

HIT_ACCUMULATOR = 1 << 0
HIT_LINEAR = 1 << 1
HIT_ANGULAR = 1 << 2
HIT_POSE = 1 << 3
KNOWN_EVENT_FLAGS = HIT_ACCUMULATOR | HIT_LINEAR | HIT_ANGULAR | HIT_POSE

STATE_KINDS = (
    ("native_linear", HIT_LINEAR),
    ("native_angular", HIT_ANGULAR),
    ("downstream_pose", HIT_POSE),
)


def bits_to_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def hex64(value: int) -> str:
    return f"0x{value:x}"


def summarize_counter(counter: collections.Counter[Any]) -> dict[str, int]:
    return {str(key): counter[key] for key in sorted(counter, key=str)}


def summarize_rips(counter: collections.Counter[int], library_base: int) -> list[dict[str, Any]]:
    rows = []
    for rip, count in counter.most_common():
        rva = rip - library_base if library_base <= rip < library_base + 0x10000000 else None
        rows.append(
            {
                "rip": hex64(rip),
                "library_rva": hex64(rva) if rva is not None else None,
                "count": count,
            }
        )
    return rows


def parse_trace(
    path: Path, minimum_cycles: int, include_all_gaps: bool = False
) -> dict[str, Any]:
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError(f"trace is truncated: {len(raw)} < {HEADER.size}")

    values = HEADER.unpack_from(raw)
    (
        magic,
        version,
        header_size,
        event_size,
        header_flags,
        pid,
        library_base,
        main_object,
        accumulator_address,
        native_body,
        native_linear_address,
        native_angular_address,
        downstream_pose_address,
        downstream_rotation_address,
        start_ns,
        declared_event_count,
        declared_accumulator_hits,
        declared_linear_hits,
        declared_angular_hits,
        declared_pose_hits,
        read_errors,
        ptrace_errors,
        unexpected_stops,
        attached_threads,
        final_threads,
    ) = values

    structural_errors: list[str] = []
    if magic != MAGIC:
        structural_errors.append(f"bad magic: {magic!r}")
    if version != VERSION:
        structural_errors.append(f"bad version: {version}")
    if header_size != HEADER.size:
        structural_errors.append(f"bad header size: {header_size} != {HEADER.size}")
    if event_size != EVENT.size:
        structural_errors.append(f"bad event size: {event_size} != {EVENT.size}")

    expected_size = header_size + declared_event_count * event_size
    if expected_size != len(raw):
        structural_errors.append(
            f"file size mismatch: {len(raw)} != declared {expected_size}"
        )
    if structural_errors:
        return {
            "schema": "a9tas.zero_gap_assessment.v1",
            "path": str(path.resolve()),
            "structurally_valid": False,
            "structural_errors": structural_errors,
        }

    events = []
    decoded_hits = collections.Counter()
    invalid_float_samples = collections.Counter()
    unreadable_events = 0
    previous_sequence = None
    previous_time = None
    sequence_errors = 0
    time_regressions = 0
    unknown_flag_events = 0
    offset = header_size
    for index in range(declared_event_count):
        unpacked = EVENT.unpack_from(raw, offset + index * event_size)
        (
            sequence,
            monotonic_ns,
            tid,
            flags,
            rip,
            accumulator,
            linear_bits,
            angular_bits,
            pose_bits,
            read_ok,
        ) = unpacked
        if previous_sequence is not None and sequence != previous_sequence + 1:
            sequence_errors += 1
        if previous_time is not None and monotonic_ns < previous_time:
            time_regressions += 1
        previous_sequence = sequence
        previous_time = monotonic_ns
        unknown_flag_events += int(bool(flags & ~KNOWN_EVENT_FLAGS))
        unreadable_events += int(read_ok != 1)
        for name, flag in (
            ("accumulator", HIT_ACCUMULATOR),
            ("native_linear", HIT_LINEAR),
            ("native_angular", HIT_ANGULAR),
            ("downstream_pose", HIT_POSE),
        ):
            if flags & flag:
                decoded_hits[name] += 1
        decoded_values = {
            "native_linear": bits_to_float(linear_bits),
            "native_angular": bits_to_float(angular_bits),
            "downstream_pose": bits_to_float(pose_bits),
        }
        if read_ok == 1:
            for name, value in decoded_values.items():
                if not math.isfinite(value):
                    invalid_float_samples[name] += 1
        events.append(
            {
                "sequence": sequence,
                "monotonic_ns": monotonic_ns,
                "tid": tid,
                "flags": flags,
                "rip": rip,
                "accumulator": accumulator,
                "read_ok": read_ok == 1,
                "values": decoded_values,
            }
        )

    declared_hits = {
        "accumulator": declared_accumulator_hits,
        "native_linear": declared_linear_hits,
        "native_angular": declared_angular_hits,
        "downstream_pose": declared_pose_hits,
    }
    hit_count_mismatches = {
        name: {"declared": declared, "decoded": decoded_hits[name]}
        for name, declared in declared_hits.items()
        if declared != decoded_hits[name]
    }

    phase = "unknown"
    complete_active_to_zero = 0
    closed_zero_gaps = 0
    open_zero_gap: dict[str, Any] | None = None
    zero_gaps: list[dict[str, Any]] = []
    state = {
        name: {
            "total_hits": 0,
            "phase_active_hits": 0,
            "phase_zero_hits": 0,
            "phase_unknown_hits": 0,
            "sampled_accumulator_zero_hits": 0,
            "sampled_accumulator_nonzero_hits": 0,
            "unreadable_hits": 0,
            "tid_counts": collections.Counter(),
            "rip_counts": collections.Counter(),
        }
        for name, _ in STATE_KINDS
    }
    phase_sample_disagreements = 0

    for event in events:
        if event["flags"] & HIT_ACCUMULATOR:
            next_phase = "active" if event["accumulator"] != 0 else "zero"
            if phase == "active" and next_phase == "zero":
                complete_active_to_zero += 1
                open_zero_gap = {
                    "start_sequence": event["sequence"],
                    "start_ns": event["monotonic_ns"],
                    "end_sequence": None,
                    "duration_us": None,
                    "writes": {name: 0 for name, _ in STATE_KINDS},
                }
            elif phase == "zero" and next_phase == "active" and open_zero_gap is not None:
                open_zero_gap["end_sequence"] = event["sequence"]
                open_zero_gap["duration_us"] = round(
                    (event["monotonic_ns"] - open_zero_gap["start_ns"]) / 1000.0,
                    3,
                )
                zero_gaps.append(open_zero_gap)
                open_zero_gap = None
                closed_zero_gaps += 1
            phase = next_phase

        for name, flag in STATE_KINDS:
            if not event["flags"] & flag:
                continue
            item = state[name]
            item["total_hits"] += 1
            item[f"phase_{phase}_hits"] += 1
            item["tid_counts"][event["tid"]] += 1
            item["rip_counts"][event["rip"]] += 1
            if not event["read_ok"]:
                item["unreadable_hits"] += 1
            sampled_phase = "zero" if event["accumulator"] == 0 else "active"
            item[f"sampled_accumulator_{'zero' if sampled_phase == 'zero' else 'nonzero'}_hits"] += 1
            if phase != "unknown" and sampled_phase != phase:
                phase_sample_disagreements += 1
            if phase == "zero" and open_zero_gap is not None:
                open_zero_gap["writes"][name] += 1

    if open_zero_gap is not None:
        zero_gaps.append(open_zero_gap)

    zero_gap_writes = {
        name: sum(gap["writes"][name] for gap in zero_gaps)
        for name, _ in STATE_KINDS
    }
    sampled_zero_writes = {
        name: state[name]["sampled_accumulator_zero_hits"]
        for name, _ in STATE_KINDS
    }
    unknown_phase_writes = {
        name: state[name]["phase_unknown_hits"] for name, _ in STATE_KINDS
    }
    watched_all_hit = all(state[name]["total_hits"] > 0 for name, _ in STATE_KINDS)
    enough_cycles = closed_zero_gaps >= minimum_cycles

    transport_clean = all(
        (
            bool(header_flags & HEADER_CLEAN),
            bool(header_flags & HEADER_TARGET_VERIFIED),
            read_errors == 0,
            ptrace_errors == 0,
            unexpected_stops == 0,
            unreadable_events == 0,
            attached_threads == final_threads,
            sequence_errors == 0,
            time_regressions == 0,
            unknown_flag_events == 0,
            not hit_count_mismatches,
        )
    )
    no_zero_window_writes = not any(zero_gap_writes.values())
    no_sampled_zero_writes = not any(sampled_zero_writes.values())
    no_unknown_phase_writes = not any(unknown_phase_writes.values())
    quiescence_supported = all(
        (
            transport_clean,
            watched_all_hit,
            enough_cycles,
            no_zero_window_writes,
            no_sampled_zero_writes,
            no_unknown_phase_writes,
            phase_sample_disagreements == 0,
        )
    )

    state_output = {}
    for name, _ in STATE_KINDS:
        item = state[name]
        state_output[name] = {
            key: value
            for key, value in item.items()
            if key not in ("tid_counts", "rip_counts")
        }
        state_output[name]["tid_counts"] = summarize_counter(item["tid_counts"])
        state_output[name]["writers"] = summarize_rips(
            item["rip_counts"], library_base
        )

    closed_durations = [
        gap["duration_us"] for gap in zero_gaps if gap["duration_us"] is not None
    ]
    duration_summary = None
    if closed_durations:
        duration_summary = {
            "minimum_us": min(closed_durations),
            "maximum_us": max(closed_durations),
            "mean_us": round(sum(closed_durations) / len(closed_durations), 3),
        }

    gap_samples = []
    sampled_sequences = set()
    for gap in zero_gaps[:3] + zero_gaps[-3:]:
        if gap["start_sequence"] not in sampled_sequences:
            gap_samples.append(gap)
            sampled_sequences.add(gap["start_sequence"])
    violating_gaps = [gap for gap in zero_gaps if any(gap["writes"].values())]
    violating_gap_samples = []
    violating_sample_sequences = set()
    for gap in violating_gaps[:3] + violating_gaps[-3:]:
        if gap["start_sequence"] not in violating_sample_sequences:
            violating_gap_samples.append(gap)
            violating_sample_sequences.add(gap["start_sequence"])

    phase_output = {
        "minimum_required_closed_zero_gaps": minimum_cycles,
        "active_to_zero_transitions": complete_active_to_zero,
        "closed_zero_gaps": closed_zero_gaps,
        "zero_gap_duration": duration_summary,
        "zero_gap_writes": zero_gap_writes,
        "sampled_accumulator_zero_writes": sampled_zero_writes,
        "unknown_phase_writes": unknown_phase_writes,
        "phase_sample_disagreements": phase_sample_disagreements,
        "gap_samples": gap_samples,
        "violating_gap_count": len(violating_gaps),
        "violating_gap_samples": violating_gap_samples,
    }
    if include_all_gaps:
        phase_output["gaps"] = zero_gaps

    return {
        "schema": "a9tas.zero_gap_assessment.v1",
        "path": str(path.resolve()),
        "structurally_valid": True,
        "header": {
            "pid": pid,
            "library_base": hex64(library_base),
            "main_object": hex64(main_object),
            "accumulator_address": hex64(accumulator_address),
            "native_body": hex64(native_body),
            "native_linear_address": hex64(native_linear_address),
            "native_angular_address": hex64(native_angular_address),
            "downstream_pose_address": hex64(downstream_pose_address),
            "downstream_rotation_address": hex64(downstream_rotation_address),
            "watch_kind": "rotation" if header_flags & HEADER_ROTATION_WATCH else "position",
            "start_ns": start_ns,
            "event_count": declared_event_count,
            "attached_threads": attached_threads,
            "final_threads": final_threads,
            "clean_flag": bool(header_flags & HEADER_CLEAN),
            "target_verified_flag": bool(header_flags & HEADER_TARGET_VERIFIED),
            "read_errors": read_errors,
            "ptrace_errors": ptrace_errors,
            "unexpected_stops": unexpected_stops,
        },
        "integrity": {
            "transport_clean": transport_clean,
            "unreadable_events": unreadable_events,
            "sequence_errors": sequence_errors,
            "time_regressions": time_regressions,
            "unknown_flag_events": unknown_flag_events,
            "invalid_float_samples": summarize_counter(invalid_float_samples),
            "hit_count_mismatches": hit_count_mismatches,
        },
        "phase_analysis": phase_output,
        "state_writes": state_output,
        "assessment": {
            "watched_all_hit": watched_all_hit,
            "enough_cycles": enough_cycles,
            "no_ordered_zero_gap_writes": no_zero_window_writes,
            "no_sampled_zero_writes": no_sampled_zero_writes,
            "no_unknown_phase_writes": no_unknown_phase_writes,
            "quiescence_supported": quiescence_supported,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--minimum-cycles", type=int, default=20)
    parser.add_argument("--include-all-gaps", action="store_true")
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    if args.minimum_cycles < 1:
        parser.error("--minimum-cycles must be positive")
    try:
        report = parse_trace(
            args.trace, args.minimum_cycles, include_all_gaps=args.include_all_gaps
        )
    except (OSError, ValueError, struct.error) as exc:
        print(json.dumps({"error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 2
    print(
        json.dumps(
            report,
            ensure_ascii=False,
            indent=None if args.compact else 2,
            sort_keys=False,
        )
    )
    return 0 if report.get("structurally_valid") else 2


if __name__ == "__main__":
    raise SystemExit(main())
