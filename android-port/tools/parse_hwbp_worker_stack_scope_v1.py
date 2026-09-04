#!/usr/bin/env python3
"""Parse A9WSS1 traces and assess the host-only worker stack scope."""

from __future__ import annotations

import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any


HEADER = struct.Struct("<8sIIII" + "Q" * 18 + "IIII")
EVENT = struct.Struct("<QQiIQQ" + "I" * 6 + "Q" * 32)
MAGIC = b"A9WSS1\0\0"
VERSION = 1

HEADER_CLEAN = 1 << 0
HEADER_TARGET_VERIFIED = 1 << 1
HIT_LINEAR = 1 << 0
HIT_ANGULAR = 1 << 1
HIT_POSE = 1 << 2
KNOWN_FLAGS = HIT_LINEAR | HIT_ANGULAR | HIT_POSE
EXPECTED_PHASE_RETURN_RVAS = (0x5E080A0, 0x5E080AC, 0x5E080B8)
STATE_KINDS = (
    ("native_linear", HIT_LINEAR, "linear"),
    ("native_angular", HIT_ANGULAR, "angular"),
    ("native_pose_tail", HIT_POSE, "pose"),
)


def bits_to_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def hex64(value: int) -> str:
    return f"0x{value:x}"


def counter_rows(counter: collections.Counter[int]) -> list[dict[str, Any]]:
    return [
        {"value": hex64(value), "count": count}
        for value, count in counter.most_common()
    ]


def parse_trace(path: Path, minimum_hits_per_field: int = 20) -> dict[str, Any]:
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
        native_body,
        linear_address,
        angular_address,
        pose_address,
        phase_return_0,
        phase_return_1,
        phase_return_2,
        start_ns,
        declared_events,
        declared_linear_hits,
        declared_angular_hits,
        declared_pose_hits,
        value_read_errors,
        stack_read_errors,
        ptrace_errors,
        unexpected_stops,
        worker_tid,
        attached_threads,
        final_threads,
        header_reserved,
    ) = values

    structural_errors = []
    if magic != MAGIC:
        structural_errors.append(f"bad magic: {magic!r}")
    if version != VERSION:
        structural_errors.append(f"bad version: {version}")
    if header_size != HEADER.size:
        structural_errors.append(
            f"bad header size: {header_size} != {HEADER.size}"
        )
    if event_size != EVENT.size:
        structural_errors.append(
            f"bad event size: {event_size} != {EVENT.size}"
        )
    expected_size = header_size + declared_events * event_size
    if expected_size != len(raw):
        structural_errors.append(
            f"file size mismatch: {len(raw)} != {expected_size}"
        )
    if header_reserved != 0:
        structural_errors.append(
            f"nonzero reserved header field: {header_reserved}"
        )
    if structural_errors:
        return {
            "schema": "a9tas.worker_stack_scope_assessment.v1",
            "path": str(path.resolve()),
            "structurally_valid": False,
            "structural_errors": structural_errors,
        }

    phase_returns = (phase_return_0, phase_return_1, phase_return_2)
    expected_phase_returns = tuple(
        library_base + rva for rva in EXPECTED_PHASE_RETURN_RVAS
    )
    boundary_addresses_match = phase_returns == expected_phase_returns

    state = {
        name: {
            "total_hits": 0,
            "scoped_hits": 0,
            "missing_phase_marker_hits": 0,
            "ambiguous_phase_marker_hits": 0,
            "unreadable_value_hits": 0,
            "unreadable_stack_hits": 0,
            "invalid_float_hits": 0,
            "writers": collections.Counter(),
            "phase_markers": collections.Counter(),
            "phase_marker_indices": collections.Counter(),
        }
        for name, _, _ in STATE_KINDS
    }
    decoded_hits = collections.Counter()
    wrong_tid_events = 0
    sequence_errors = 0
    time_regressions = 0
    unknown_flag_events = 0
    empty_flag_events = 0
    unreadable_value_events = 0
    unreadable_stack_events = 0
    nonzero_event_reserved = 0
    previous_sequence = None
    previous_time = None

    for index in range(declared_events):
        event = EVENT.unpack_from(raw, header_size + index * event_size)
        (
            sequence,
            monotonic_ns,
            tid,
            event_flags,
            rip,
            rsp,
            linear_bits,
            angular_bits,
            pose_bits,
            value_read_ok,
            stack_read_ok,
            event_reserved,
            *stack_words,
        ) = event
        del rsp
        if previous_sequence is not None and sequence != previous_sequence + 1:
            sequence_errors += 1
        if previous_time is not None and monotonic_ns < previous_time:
            time_regressions += 1
        previous_sequence = sequence
        previous_time = monotonic_ns
        wrong_tid_events += int(tid != worker_tid)
        unknown_flag_events += int(bool(event_flags & ~KNOWN_FLAGS))
        empty_flag_events += int(not bool(event_flags & KNOWN_FLAGS))
        unreadable_value_events += int(value_read_ok != 1)
        unreadable_stack_events += int(stack_read_ok != 1)
        nonzero_event_reserved += int(event_reserved != 0)

        phase_occurrences = [
            (stack_index, phase_returns.index(word), word)
            for stack_index, word in enumerate(stack_words)
            if word in phase_returns
        ] if stack_read_ok == 1 else []
        values_by_kind = {
            "linear": bits_to_float(linear_bits),
            "angular": bits_to_float(angular_bits),
            "pose": bits_to_float(pose_bits),
        }

        for name, state_flag, value_key in STATE_KINDS:
            if not event_flags & state_flag:
                continue
            decoded_hits[name] += 1
            item = state[name]
            item["total_hits"] += 1
            item["writers"][rip] += 1
            item["unreadable_value_hits"] += int(value_read_ok != 1)
            item["unreadable_stack_hits"] += int(stack_read_ok != 1)
            if value_read_ok == 1 and not math.isfinite(
                values_by_kind[value_key]
            ):
                item["invalid_float_hits"] += 1
            if stack_read_ok != 1:
                continue
            if len(phase_occurrences) == 0:
                item["missing_phase_marker_hits"] += 1
            elif len(phase_occurrences) > 1:
                item["ambiguous_phase_marker_hits"] += 1
            if len(phase_occurrences) == 1:
                item["scoped_hits"] += 1
                marker_index, phase_index, marker = phase_occurrences[0]
                item["phase_markers"][marker] += 1
                item["phase_marker_indices"][(phase_index, marker_index)] += 1

    declared_hits = {
        "native_linear": declared_linear_hits,
        "native_angular": declared_angular_hits,
        "native_pose_tail": declared_pose_hits,
    }
    hit_mismatches = {
        name: {"declared": count, "decoded": decoded_hits[name]}
        for name, count in declared_hits.items()
        if count != decoded_hits[name]
    }
    transport_clean = all(
        (
            bool(flags & HEADER_CLEAN),
            bool(flags & HEADER_TARGET_VERIFIED),
            boundary_addresses_match,
            value_read_errors == 0,
            stack_read_errors == 0,
            ptrace_errors == 0,
            unexpected_stops == 0,
            unreadable_value_events == 0,
            unreadable_stack_events == 0,
            wrong_tid_events == 0,
            sequence_errors == 0,
            time_regressions == 0,
            unknown_flag_events == 0,
            empty_flag_events == 0,
            nonzero_event_reserved == 0,
            attached_threads == 1,
            final_threads == 1,
            not hit_mismatches,
        )
    )

    state_output = {}
    all_fields_enough = True
    all_writes_scoped = True
    for name, _, _ in STATE_KINDS:
        item = state[name]
        total = item["total_hits"]
        field_enough = total >= minimum_hits_per_field
        field_scoped = (
            item["scoped_hits"] == total
            and item["missing_phase_marker_hits"] == 0
            and item["ambiguous_phase_marker_hits"] == 0
            and item["unreadable_value_hits"] == 0
            and item["unreadable_stack_hits"] == 0
            and item["invalid_float_hits"] == 0
        )
        all_fields_enough = all_fields_enough and field_enough
        all_writes_scoped = all_writes_scoped and field_scoped
        state_output[name] = {
            key: value
            for key, value in item.items()
            if key not in (
                "writers",
                "phase_markers",
                "phase_marker_indices",
            )
        }
        state_output[name]["minimum_required_hits"] = minimum_hits_per_field
        state_output[name]["enough_hits"] = field_enough
        state_output[name]["all_writes_scoped"] = field_scoped
        state_output[name]["writers"] = counter_rows(item["writers"])
        state_output[name]["phase_markers"] = counter_rows(
            item["phase_markers"]
        )
        state_output[name]["phase_marker_indices"] = [
            {
                "phase": phase_index,
                "stack_index": stack_index,
                "count": count,
            }
            for (phase_index, stack_index), count
            in item["phase_marker_indices"].most_common()
        ]

    boundary_supported = transport_clean and all_fields_enough and all_writes_scoped
    return {
        "schema": "a9tas.worker_stack_scope_assessment.v1",
        "path": str(path.resolve()),
        "structurally_valid": True,
        "header": {
            "pid": pid,
            "library_base": hex64(library_base),
            "native_body": hex64(native_body),
            "native_linear_address": hex64(linear_address),
            "native_angular_address": hex64(angular_address),
            "native_pose_tail_address": hex64(pose_address),
            "phase_returns": [hex64(value) for value in phase_returns],
            "start_ns": start_ns,
            "event_count": declared_events,
            "worker_tid": worker_tid,
            "clean_flag": bool(flags & HEADER_CLEAN),
            "target_verified_flag": bool(flags & HEADER_TARGET_VERIFIED),
            "value_read_errors": value_read_errors,
            "stack_read_errors": stack_read_errors,
            "ptrace_errors": ptrace_errors,
            "unexpected_stops": unexpected_stops,
        },
        "integrity": {
            "transport_clean": transport_clean,
            "boundary_addresses_match": boundary_addresses_match,
            "unreadable_value_events": unreadable_value_events,
            "unreadable_stack_events": unreadable_stack_events,
            "wrong_tid_events": wrong_tid_events,
            "sequence_errors": sequence_errors,
            "time_regressions": time_regressions,
            "unknown_flag_events": unknown_flag_events,
            "empty_flag_events": empty_flag_events,
            "nonzero_event_reserved": nonzero_event_reserved,
            "hit_count_mismatches": hit_mismatches,
        },
        "state_writes": state_output,
        "assessment": {
            "all_fields_enough": all_fields_enough,
            "all_writes_scoped": all_writes_scoped,
            "boundary_supported": boundary_supported,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--minimum-hits-per-field", type=int, default=20)
    parser.add_argument("--compact", action="store_true")
    parser.add_argument(
        "--require-supported",
        action="store_true",
        help="return a nonzero status unless the semantic boundary passes",
    )
    args = parser.parse_args()
    if args.minimum_hits_per_field < 1:
        parser.error("--minimum-hits-per-field must be positive")
    try:
        report = parse_trace(args.trace, args.minimum_hits_per_field)
    except (OSError, ValueError, struct.error) as exc:
        print(json.dumps({"error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False,
                     indent=None if args.compact else 2))
    if not report.get("structurally_valid"):
        return 2
    if args.require_supported and not report.get("assessment", {}).get(
        "boundary_supported", False
    ):
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
