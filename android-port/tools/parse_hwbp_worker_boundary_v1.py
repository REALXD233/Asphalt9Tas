#!/usr/bin/env python3
"""Parse an A9WBP1 trace and assess the worker callback boundary."""

from __future__ import annotations

import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any


HEADER = struct.Struct("<8sIIII" + "Q" * 16 + "IIII")
EVENT = struct.Struct("<QQiIQQIIII")
MAGIC = b"A9WBP1\0\0"
VERSION = 1

HEADER_CLEAN = 1 << 0
HEADER_TARGET_VERIFIED = 1 << 1
HIT_PHASE = 1 << 0
HIT_LINEAR = 1 << 1
HIT_ANGULAR = 1 << 2
HIT_POSE = 1 << 3
KNOWN_FLAGS = HIT_PHASE | HIT_LINEAR | HIT_ANGULAR | HIT_POSE
STATE_KINDS = (
    ("native_linear", HIT_LINEAR, "linear"),
    ("native_angular", HIT_ANGULAR, "angular"),
    ("native_pose_tail", HIT_POSE, "pose"),
)


def bits_to_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def hex64(value: int) -> str:
    return f"0x{value:x}"


def writer_rows(counter: collections.Counter[int]) -> list[dict[str, Any]]:
    return [
        {"rip": hex64(rip), "count": count}
        for rip, count in counter.most_common()
    ]


def parse_trace(path: Path, minimum_pairs: int = 20) -> dict[str, Any]:
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError(f"trace truncated: {len(raw)} < {HEADER.size}")
    values = HEADER.unpack_from(raw)
    (
        magic,
        version,
        header_size,
        event_size,
        flags,
        pid,
        library_base,
        active_address,
        native_body,
        linear_address,
        angular_address,
        pose_address,
        start_ns,
        declared_events,
        declared_phase_hits,
        declared_linear_hits,
        declared_angular_hits,
        declared_pose_hits,
        read_errors,
        ptrace_errors,
        unexpected_stops,
        worker_tid,
        attached_threads,
        final_threads,
        reserved,
    ) = values

    errors = []
    if magic != MAGIC:
        errors.append(f"bad magic: {magic!r}")
    if version != VERSION:
        errors.append(f"bad version: {version}")
    if header_size != HEADER.size:
        errors.append(f"bad header size: {header_size} != {HEADER.size}")
    if event_size != EVENT.size:
        errors.append(f"bad event size: {event_size} != {EVENT.size}")
    expected_size = header_size + declared_events * event_size
    if expected_size != len(raw):
        errors.append(f"file size mismatch: {len(raw)} != {expected_size}")
    if reserved != 0:
        errors.append(f"nonzero reserved header field: {reserved}")
    if errors:
        return {
            "schema": "a9tas.worker_boundary_assessment.v1",
            "path": str(path.resolve()),
            "structurally_valid": False,
            "structural_errors": errors,
        }

    decoded_hits = collections.Counter()
    phase_values = collections.Counter()
    state = {
        name: {
            "total_hits": 0,
            "active_hits": 0,
            "inactive_hits": 0,
            "unreadable_hits": 0,
            "invalid_float_hits": 0,
            "writers": collections.Counter(),
        }
        for name, _, _ in STATE_KINDS
    }
    unreadable_events = 0
    wrong_tid_events = 0
    sequence_errors = 0
    time_regressions = 0
    unknown_flag_events = 0
    previous_sequence = None
    previous_time = None
    offset = header_size
    for index in range(declared_events):
        (
            sequence,
            monotonic_ns,
            tid,
            event_flags,
            rip,
            active_calls,
            linear_bits,
            angular_bits,
            pose_bits,
            read_ok,
        ) = EVENT.unpack_from(raw, offset + index * event_size)
        if previous_sequence is not None and sequence != previous_sequence + 1:
            sequence_errors += 1
        if previous_time is not None and monotonic_ns < previous_time:
            time_regressions += 1
        previous_sequence = sequence
        previous_time = monotonic_ns
        wrong_tid_events += int(tid != worker_tid)
        unknown_flag_events += int(bool(event_flags & ~KNOWN_FLAGS))
        unreadable_events += int(read_ok != 1)
        values_by_kind = {
            "linear": bits_to_float(linear_bits),
            "angular": bits_to_float(angular_bits),
            "pose": bits_to_float(pose_bits),
        }
        if event_flags & HIT_PHASE:
            decoded_hits["phase"] += 1
            phase_values[active_calls] += 1
        for name, state_flag, value_key in STATE_KINDS:
            if not event_flags & state_flag:
                continue
            decoded_hits[name] += 1
            item = state[name]
            item["total_hits"] += 1
            item["active_hits" if active_calls > 0 else "inactive_hits"] += 1
            item["unreadable_hits"] += int(read_ok != 1)
            if read_ok == 1 and not math.isfinite(values_by_kind[value_key]):
                item["invalid_float_hits"] += 1
            item["writers"][rip] += 1

    declared_hits = {
        "phase": declared_phase_hits,
        "native_linear": declared_linear_hits,
        "native_angular": declared_angular_hits,
        "native_pose_tail": declared_pose_hits,
    }
    hit_mismatches = {
        name: {"declared": count, "decoded": decoded_hits[name]}
        for name, count in declared_hits.items()
        if count != decoded_hits[name]
    }
    positive_phase_hits = sum(
        count for active, count in phase_values.items() if active > 0
    )
    zero_phase_hits = phase_values[0]
    complete_pairs = min(positive_phase_hits, zero_phase_hits)
    phase_balance = abs(positive_phase_hits - zero_phase_hits)
    max_active_calls = max(phase_values, default=0)
    all_state_fields_hit = all(
        state[name]["total_hits"] > 0 for name, _, _ in STATE_KINDS
    )
    inactive_state_writes = {
        name: state[name]["inactive_hits"] for name, _, _ in STATE_KINDS
    }
    transport_clean = all(
        (
            bool(flags & HEADER_CLEAN),
            bool(flags & HEADER_TARGET_VERIFIED),
            read_errors == 0,
            ptrace_errors == 0,
            unexpected_stops == 0,
            unreadable_events == 0,
            wrong_tid_events == 0,
            sequence_errors == 0,
            time_regressions == 0,
            unknown_flag_events == 0,
            attached_threads == 1,
            final_threads == 1,
            not hit_mismatches,
        )
    )
    boundary_supported = all(
        (
            transport_clean,
            complete_pairs >= minimum_pairs,
            phase_balance <= 1,
            max_active_calls == 1,
            all_state_fields_hit,
            not any(inactive_state_writes.values()),
        )
    )

    state_output = {}
    for name, _, _ in STATE_KINDS:
        item = state[name]
        state_output[name] = {
            key: value for key, value in item.items() if key != "writers"
        }
        state_output[name]["writers"] = writer_rows(item["writers"])

    return {
        "schema": "a9tas.worker_boundary_assessment.v1",
        "path": str(path.resolve()),
        "structurally_valid": True,
        "header": {
            "pid": pid,
            "library_base": hex64(library_base),
            "active_address": hex64(active_address),
            "native_body": hex64(native_body),
            "native_linear_address": hex64(linear_address),
            "native_angular_address": hex64(angular_address),
            "native_pose_tail_address": hex64(pose_address),
            "start_ns": start_ns,
            "event_count": declared_events,
            "worker_tid": worker_tid,
            "clean_flag": bool(flags & HEADER_CLEAN),
            "target_verified_flag": bool(flags & HEADER_TARGET_VERIFIED),
            "read_errors": read_errors,
            "ptrace_errors": ptrace_errors,
            "unexpected_stops": unexpected_stops,
        },
        "integrity": {
            "transport_clean": transport_clean,
            "unreadable_events": unreadable_events,
            "wrong_tid_events": wrong_tid_events,
            "sequence_errors": sequence_errors,
            "time_regressions": time_regressions,
            "unknown_flag_events": unknown_flag_events,
            "hit_count_mismatches": hit_mismatches,
        },
        "phase": {
            "minimum_required_pairs": minimum_pairs,
            "positive_phase_hits": positive_phase_hits,
            "zero_phase_hits": zero_phase_hits,
            "complete_pairs": complete_pairs,
            "phase_balance": phase_balance,
            "max_active_calls": max_active_calls,
            "value_histogram": {
                str(value): phase_values[value] for value in sorted(phase_values)
            },
        },
        "state_writes": state_output,
        "assessment": {
            "all_state_fields_hit": all_state_fields_hit,
            "inactive_state_writes": inactive_state_writes,
            "enough_pairs": complete_pairs >= minimum_pairs,
            "single_worker_scope": max_active_calls == 1,
            "boundary_supported": boundary_supported,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--minimum-pairs", type=int, default=20)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    if args.minimum_pairs < 1:
        parser.error("--minimum-pairs must be positive")
    try:
        report = parse_trace(args.trace, args.minimum_pairs)
    except (OSError, ValueError, struct.error) as exc:
        print(json.dumps({"error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False,
                     indent=None if args.compact else 2))
    return 0 if report.get("structurally_valid") else 2


if __name__ == "__main__":
    raise SystemExit(main())
