#!/usr/bin/env python3
"""Parse A9ESA1 host-only simulation-executor affinity traces."""

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
STACK_WORDS = 256
EVENT = struct.Struct("<QQiIQQ" + "I" * 6 + "Q" * STACK_WORDS)
MAGIC = b"A9ESA1\0\0"
VERSION = 1

HEADER_CLEAN = 1 << 0
HEADER_TARGET_VERIFIED = 1 << 1
HIT_POSE = 1 << 2
EXPECTED_PHASE_RETURN_RVAS = (0x5E080A0, 0x5E080AC, 0x5E080B8)
EXPECTED_EXECUTOR_RETURN_RVA = 0x38B7578


def hex64(value: int) -> str:
    return f"0x{value:x}"


def parse_trace(path: Path, minimum_hits: int = 20) -> dict[str, Any]:
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError(f"trace truncated: {len(raw)} < {HEADER.size}")
    values = HEADER.unpack_from(raw)
    (
        magic, version, header_size, event_size, flags, pid, library_base,
        native_body, linear_address, angular_address, pose_address,
        phase_return_0, phase_return_1, phase_return_2, start_ns,
        declared_events, declared_linear_hits, declared_angular_hits,
        declared_pose_hits, value_read_errors, stack_read_errors,
        ptrace_errors, unexpected_stops, worker_tid, attached_threads,
        final_threads, header_reserved,
    ) = values

    structural_errors: list[str] = []
    if magic != MAGIC:
        structural_errors.append(f"bad magic: {magic!r}")
    if version != VERSION:
        structural_errors.append(f"bad version: {version}")
    if header_size != HEADER.size:
        structural_errors.append(f"bad header size: {header_size}")
    if event_size != EVENT.size:
        structural_errors.append(f"bad event size: {event_size}")
    if header_size + declared_events * event_size != len(raw):
        structural_errors.append("file size mismatch")
    if header_reserved != 0:
        structural_errors.append("nonzero reserved header field")
    if structural_errors:
        return {
            "schema": "a9tas.executor_stack_affinity.v1",
            "path": str(path.resolve()),
            "structurally_valid": False,
            "structural_errors": structural_errors,
        }

    phase_returns = (phase_return_0, phase_return_1, phase_return_2)
    expected_phase_returns = tuple(
        library_base + rva for rva in EXPECTED_PHASE_RETURN_RVAS
    )
    executor_return = library_base + EXPECTED_EXECUTOR_RETURN_RVA
    phase_indices: collections.Counter[tuple[int, int]] = collections.Counter()
    executor_indices: collections.Counter[int] = collections.Counter()
    writer_rips: collections.Counter[int] = collections.Counter()
    phase_qualified_hits = 0
    non_phase_writer_hits = 0
    ambiguous_phase = 0
    unique_executor_hits = 0
    missing_executor = 0
    ambiguous_executor = 0
    qualified_missing_executor = 0
    qualified_ambiguous_executor = 0
    non_phase_executor_hits = 0
    invalid_float_events = 0
    wrong_tid_events = 0
    sequence_errors = 0
    time_regressions = 0
    bad_flag_events = 0
    unreadable_value_events = 0
    unreadable_stack_events = 0
    nonzero_event_reserved = 0
    previous_sequence: int | None = None
    previous_time: int | None = None

    for index in range(declared_events):
        event = EVENT.unpack_from(raw, header_size + index * event_size)
        (
            sequence, monotonic_ns, tid, event_flags, rip, _rsp,
            _linear_bits, _angular_bits, pose_bits, value_read_ok,
            stack_read_ok, event_reserved, *stack_words,
        ) = event
        if previous_sequence is not None and sequence != previous_sequence + 1:
            sequence_errors += 1
        if previous_time is not None and monotonic_ns < previous_time:
            time_regressions += 1
        previous_sequence = sequence
        previous_time = monotonic_ns
        wrong_tid_events += int(tid != worker_tid)
        bad_flag_events += int(event_flags != HIT_POSE)
        unreadable_value_events += int(value_read_ok != 1)
        unreadable_stack_events += int(stack_read_ok != 1)
        nonzero_event_reserved += int(event_reserved != 0)
        if value_read_ok == 1:
            pose_value = struct.unpack("<f", struct.pack("<I", pose_bits))[0]
            invalid_float_events += int(not math.isfinite(pose_value))
        writer_rips[rip] += 1
        if stack_read_ok != 1:
            continue
        phases = [
            (stack_index, phase_returns.index(word))
            for stack_index, word in enumerate(stack_words)
            if word in phase_returns
        ]
        executors = [
            stack_index for stack_index, word in enumerate(stack_words)
            if word == executor_return
        ]
        unique_executor_hits += int(len(executors) == 1)
        missing_executor += int(len(executors) == 0)
        ambiguous_executor += int(len(executors) > 1)
        if len(phases) == 0:
            non_phase_writer_hits += 1
            non_phase_executor_hits += int(len(executors) == 1)
        elif len(phases) == 1:
            phase_qualified_hits += 1
            qualified_missing_executor += int(len(executors) == 0)
            qualified_ambiguous_executor += int(len(executors) > 1)
        else:
            ambiguous_phase += 1
        if len(phases) == 1:
            phase_indices[(phases[0][1], phases[0][0])] += 1
        if len(phases) == 1 and len(executors) == 1:
            executor_indices[executors[0]] += 1

    hit_counts_match = (
        declared_events == declared_pose_hits
        and declared_linear_hits == 0
        and declared_angular_hits == 0
    )
    transport_clean = all((
        bool(flags & HEADER_CLEAN),
        bool(flags & HEADER_TARGET_VERIFIED),
        phase_returns == expected_phase_returns,
        value_read_errors == 0,
        stack_read_errors == 0,
        ptrace_errors == 0,
        unexpected_stops == 0,
        wrong_tid_events == 0,
        sequence_errors == 0,
        time_regressions == 0,
        bad_flag_events == 0,
        unreadable_value_events == 0,
        unreadable_stack_events == 0,
        invalid_float_events == 0,
        nonzero_event_reserved == 0,
        attached_threads == 1,
        final_threads == 1,
        hit_counts_match,
    ))
    semantic_clean = all((
        phase_qualified_hits >= minimum_hits,
        ambiguous_phase == 0,
        missing_executor == 0,
        ambiguous_executor == 0,
    ))
    supported = transport_clean and semantic_clean
    return {
        "schema": "a9tas.executor_stack_affinity.v1",
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
            "executor_return": hex64(executor_return),
            "start_ns": start_ns,
            "event_count": declared_events,
            "worker_tid": worker_tid,
            "stack_words": STACK_WORDS,
        },
        "integrity": {
            "transport_clean": transport_clean,
            "hit_counts_match": hit_counts_match,
            "wrong_tid_events": wrong_tid_events,
            "sequence_errors": sequence_errors,
            "time_regressions": time_regressions,
            "bad_flag_events": bad_flag_events,
            "unreadable_value_events": unreadable_value_events,
            "unreadable_stack_events": unreadable_stack_events,
            "invalid_float_events": invalid_float_events,
            "nonzero_event_reserved": nonzero_event_reserved,
        },
        "evidence": {
            "minimum_required_phase_qualified_hits": minimum_hits,
            "phase_qualified_hits": phase_qualified_hits,
            "non_phase_writer_hits": non_phase_writer_hits,
            "ambiguous_phase_marker_hits": ambiguous_phase,
            "all_event_unique_executor_marker_hits": unique_executor_hits,
            "all_event_missing_executor_marker_hits": missing_executor,
            "all_event_ambiguous_executor_marker_hits": ambiguous_executor,
            "qualified_missing_executor_marker_hits": qualified_missing_executor,
            "qualified_ambiguous_executor_marker_hits": qualified_ambiguous_executor,
            "non_phase_executor_marker_hits": non_phase_executor_hits,
            "writer_rips": [
                {"rip": hex64(rip), "count": count}
                for rip, count in writer_rips.most_common()
            ],
            "phase_marker_indices": [
                {"phase": phase, "stack_index": stack_index, "count": count}
                for (phase, stack_index), count in phase_indices.most_common()
            ],
            "executor_marker_indices": [
                {"stack_index": stack_index, "count": count}
                for stack_index, count in executor_indices.most_common()
            ],
        },
        "assessment": {
            "semantic_clean": semantic_clean,
            "executor_affinity_supported": supported,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--minimum-hits", type=int, default=20)
    parser.add_argument("--compact", action="store_true")
    parser.add_argument("--require-supported", action="store_true")
    args = parser.parse_args()
    if args.minimum_hits < 1:
        parser.error("--minimum-hits must be positive")
    try:
        report = parse_trace(args.trace, args.minimum_hits)
    except (OSError, ValueError, struct.error) as exc:
        print(json.dumps({"error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False,
                     indent=None if args.compact else 2))
    if not report.get("structurally_valid"):
        return 2
    if args.require_supported and not report.get("assessment", {}).get(
        "executor_affinity_supported", False
    ):
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
