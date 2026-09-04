#!/usr/bin/env python3
"""Analyze raw A9VCP1 phase events without accessing a device.

The input is a packed array of 80-byte Event records from
vehicle_camera_phase_observer_protocol_v1.h.  The report describes where each
RaceView callback landed relative to the outer final-writer callback bracket.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import struct
from collections import Counter, defaultdict
from typing import Iterable


EVENT = struct.Struct("<QQIIIIIIQQQQII")
TRANSACTION_REPORT_SIZE = 376
TRANSACTION_MAXIMUM_EVENTS_OFFSET = 136
TRANSACTION_COPIED_EVENTS_OFFSET = 140
VEHICLE_BEFORE = 1
VEHICLE_AFTER = 2
CAMERA_AFTER_ORIGINAL = 4
FLAG_VEHICLE_EQUAL = 1 << 2
FLAG_VEHICLE_CORRECTED = 1 << 3
FLAG_VEHICLE_IMMEDIATE_EXACT = 1 << 4
FLAG_VEHICLE_SNAPSHOT_PRESENT = 1 << 8


@dataclasses.dataclass(frozen=True)
class Event:
    sequence: int
    monotonic_ns: int
    kind: int
    flags: int
    producer_tid: int
    vehicle_frame: int
    camera_frame: int
    reserved0: int
    vehicle_pose_hash: int
    vehicle_linear_hash: int
    camera_world_hash: int
    camera_shape_hash: int
    camera_fov_bits: int
    reserved1: int


def read_events(path: pathlib.Path, committed: int | None = None) -> list[Event]:
    data = path.read_bytes()
    if len(data) % EVENT.size != 0:
        raise ValueError(f"event file size {len(data)} is not divisible by {EVENT.size}")
    available = len(data) // EVENT.size
    count = available if committed is None else committed
    if count < 0 or count > available:
        raise ValueError(f"committed count {count} outside 0..{available}")
    return [Event(*EVENT.unpack_from(data, offset * EVENT.size))
            for offset in range(count)]


def read_transaction_events_from_bytes(data: bytes) -> list[Event]:
    if len(data) < TRANSACTION_REPORT_SIZE or data[:8] != b"A9VCPTR1":
        raise ValueError("not an A9VCPTR1 transaction report")
    version, report_size = struct.unpack_from("<II", data, 8)
    maximum_events = struct.unpack_from(
        "<I", data, TRANSACTION_MAXIMUM_EVENTS_OFFSET)[0]
    copied_events = struct.unpack_from(
        "<I", data, TRANSACTION_COPIED_EVENTS_OFFSET)[0]
    if version != 1 or report_size != TRANSACTION_REPORT_SIZE:
        raise ValueError("unsupported A9VCPTR1 report ABI")
    if maximum_events == 0 or copied_events > maximum_events:
        raise ValueError("invalid A9VCPTR1 event bounds")
    expected_size = report_size + copied_events * EVENT.size
    if len(data) != expected_size:
        raise ValueError(
            f"transaction size {len(data)} differs from expected {expected_size}")
    return [Event(*EVENT.unpack_from(data, report_size + index * EVENT.size))
            for index in range(copied_events)]


def read_transaction_events(path: pathlib.Path) -> list[Event]:
    return read_transaction_events_from_bytes(path.read_bytes())


def _equality_pattern(values: list[int]) -> str:
    labels: dict[int, int] = {}
    return "".join(str(labels.setdefault(value, len(labels)))
                   for value in values)


def analyze(events: Iterable[Event]) -> dict[str, object]:
    ordered = sorted(events, key=lambda item: item.sequence)
    valid = [item for item in ordered
             if item.kind in (VEHICLE_BEFORE, VEHICLE_AFTER,
                              CAMERA_AFTER_ORIGINAL)]
    duplicate_sequences = sum(count - 1 for count in Counter(
        item.sequence for item in valid).values() if count > 1)
    sequence_gaps = 0
    for left, right in zip(valid, valid[1:]):
        if right.sequence > left.sequence + 1:
            sequence_gaps += right.sequence - left.sequence - 1
    clock_inversions = sum(
        1 for left, right in zip(valid, valid[1:])
        if left.monotonic_ns and right.monotonic_ns
        and right.monotonic_ns < left.monotonic_ns
    )

    before_by_frame: dict[int, Event] = {}
    after_by_frame: dict[int, Event] = {}
    cameras: list[Event] = []
    duplicate_before: list[int] = []
    duplicate_after: list[int] = []
    for event in valid:
        if event.kind == VEHICLE_BEFORE:
            if event.vehicle_frame in before_by_frame:
                duplicate_before.append(event.vehicle_frame)
            else:
                before_by_frame[event.vehicle_frame] = event
        elif event.kind == VEHICLE_AFTER:
            if event.vehicle_frame in after_by_frame:
                duplicate_after.append(event.vehicle_frame)
            else:
                after_by_frame[event.vehicle_frame] = event
        else:
            cameras.append(event)

    all_frames = sorted(set(before_by_frame) | set(after_by_frame))
    frame_rows: list[dict[str, object]] = []
    phase_counts: Counter[str] = Counter()
    camera_assignments: defaultdict[int, list[tuple[str, Event]]] = defaultdict(list)

    matched = [frame for frame in all_frames
               if frame in before_by_frame and frame in after_by_frame]
    brackets = [(frame, before_by_frame[frame].sequence,
                 after_by_frame[frame].sequence) for frame in matched]
    brackets.sort(key=lambda item: item[1])

    for camera in cameras:
        placement = "outside_vehicle_brackets"
        assigned_frame = None
        for index, (frame, before_seq, after_seq) in enumerate(brackets):
            next_before = (brackets[index + 1][1]
                           if index + 1 < len(brackets) else None)
            if before_seq < camera.sequence < after_seq:
                placement = "inside_vehicle_callback"
                assigned_frame = frame
                break
            if after_seq < camera.sequence and (
                    next_before is None or camera.sequence < next_before):
                placement = "after_vehicle_before_next"
                assigned_frame = frame
                break
        phase_counts[placement] += 1
        if assigned_frame is not None:
            camera_assignments[assigned_frame].append((placement, camera))

    for frame in all_frames:
        before = before_by_frame.get(frame)
        after = after_by_frame.get(frame)
        assigned = camera_assignments.get(frame, [])
        frame_rows.append({
            "vehicle_frame": frame,
            "before_sequence": None if before is None else before.sequence,
            "after_sequence": None if after is None else after.sequence,
            "complete_bracket": before is not None and after is not None
                                and before.sequence < after.sequence,
            "equal": bool(after and after.flags & FLAG_VEHICLE_EQUAL),
            "corrected": bool(after and after.flags & FLAG_VEHICLE_CORRECTED),
            "immediate_exact": bool(after and
                                    after.flags & FLAG_VEHICLE_IMMEDIATE_EXACT),
            "camera_inside": sum(1 for phase, _ in assigned
                                 if phase == "inside_vehicle_callback"),
            "camera_after_before_next": sum(
                1 for phase, _ in assigned
                if phase == "after_vehicle_before_next"),
            "camera_frames": [event.camera_frame for _, event in assigned],
            "vehicle_tid_before": None if before is None else before.producer_tid,
            "vehicle_tid_after": None if after is None else after.producer_tid,
        })

    corrected_camera_inside = sum(
        row["camera_inside"] for row in frame_rows if row["corrected"]
    )
    equal_camera_inside = sum(
        row["camera_inside"] for row in frame_rows if row["equal"]
    )
    post_camera_snapshots = 0
    post_camera_pose_exact = 0
    post_camera_linear_exact = 0
    post_camera_both_exact = 0
    post_camera_changed = 0
    for frame, assigned in camera_assignments.items():
        after = after_by_frame.get(frame)
        if after is None:
            continue
        for placement, camera in assigned:
            if (placement != "after_vehicle_before_next" or
                    not camera.flags & FLAG_VEHICLE_SNAPSHOT_PRESENT):
                continue
            post_camera_snapshots += 1
            pose_exact = camera.vehicle_pose_hash == after.vehicle_pose_hash
            linear_exact = (
                camera.vehicle_linear_hash == after.vehicle_linear_hash)
            post_camera_pose_exact += int(pose_exact)
            post_camera_linear_exact += int(linear_exact)
            post_camera_both_exact += int(pose_exact and linear_exact)
            post_camera_changed += int(not (pose_exact and linear_exact))
    camera_count_distribution = Counter(
        len([camera for placement, camera in camera_assignments.get(frame, [])
             if placement == "after_vehicle_before_next"])
        for frame in matched[:-1]
    )
    complete_bursts: dict[int, list[Event]] = {}
    for frame in matched:
        burst = [camera for placement, camera in camera_assignments.get(frame, [])
                 if placement == "after_vehicle_before_next"]
        if len(burst) == 3:
            complete_bursts[frame] = burst
    snapshot_slot_stats = [Counter() for _ in range(3)]
    first_snapshot_change_slots: Counter[str] = Counter()
    for frame, burst in complete_bursts.items():
        after = after_by_frame.get(frame)
        if after is None:
            continue
        first_changed: int | None = None
        for slot, camera in enumerate(burst):
            if not camera.flags & FLAG_VEHICLE_SNAPSHOT_PRESENT:
                continue
            pose_exact = camera.vehicle_pose_hash == after.vehicle_pose_hash
            linear_exact = (
                camera.vehicle_linear_hash == after.vehicle_linear_hash)
            stats = snapshot_slot_stats[slot]
            stats["snapshots"] += 1
            stats["pose_exact"] += int(pose_exact)
            stats["linear_exact"] += int(linear_exact)
            stats["both_exact"] += int(pose_exact and linear_exact)
            stats["changed"] += int(not (pose_exact and linear_exact))
            if first_changed is None and not (pose_exact and linear_exact):
                first_changed = slot
        first_snapshot_change_slots[
            "none" if first_changed is None else str(first_changed)] += 1
    world_patterns = Counter(_equality_pattern([
        event.camera_world_hash for event in burst])
        for burst in complete_bursts.values())
    shape_patterns = Counter(_equality_pattern([
        event.camera_shape_hash for event in burst])
        for burst in complete_bursts.values())
    fov_patterns = Counter(_equality_pattern([
        event.camera_fov_bits for event in burst])
        for burst in complete_bursts.values())
    pre_fov_equals_previous_final = 0
    first_world_equals_previous_final = 0
    shape_equals_previous_final_world = 0
    shape_equals_same_frame_final_world = 0
    compared_burst_transitions = 0
    shape_mismatch_frames: list[int] = []
    same_slot_changes = [0, 0, 0]
    same_slot_comparisons = [0, 0, 0]
    for frame in sorted(complete_bursts):
        previous = complete_bursts.get(frame - 1)
        current = complete_bursts[frame]
        if previous is None:
            continue
        compared_burst_transitions += 1
        pre_fov_equals_previous_final += int(
            current[0].camera_fov_bits == previous[2].camera_fov_bits)
        first_world_equals_previous_final += int(
            current[0].camera_world_hash == previous[2].camera_world_hash)
        shape_matches_previous = (
            current[0].camera_shape_hash == previous[2].camera_world_hash)
        shape_equals_previous_final_world += int(shape_matches_previous)
        if not shape_matches_previous:
            shape_mismatch_frames.append(frame)
        shape_equals_same_frame_final_world += int(
            current[0].camera_shape_hash == current[2].camera_world_hash)
        for slot in range(3):
            same_slot_comparisons[slot] += 1
            same_slot_changes[slot] += int(
                current[slot].camera_world_hash !=
                previous[slot].camera_world_hash)
    return {
        "format": "A9VCP1-event-analysis-v1",
        "event_size": EVENT.size,
        "summary": {
            "valid_events": len(valid),
            "vehicle_before_events": len(before_by_frame),
            "vehicle_after_events": len(after_by_frame),
            "camera_events": len(cameras),
            "complete_vehicle_brackets": sum(
                1 for row in frame_rows if row["complete_bracket"]),
            "corrected_vehicle_frames": sum(
                1 for row in frame_rows if row["corrected"]),
            "equal_vehicle_frames": sum(1 for row in frame_rows if row["equal"]),
            "camera_phase_counts": dict(sorted(phase_counts.items())),
            "corrected_frames_camera_inside": corrected_camera_inside,
            "equal_frames_camera_inside": equal_camera_inside,
            "post_vehicle_camera_snapshots": post_camera_snapshots,
            "post_vehicle_camera_pose_exact": post_camera_pose_exact,
            "post_vehicle_camera_linear_exact": post_camera_linear_exact,
            "post_vehicle_camera_both_exact": post_camera_both_exact,
            "post_vehicle_camera_changed": post_camera_changed,
            "vehicle_tids": sorted({
                event.producer_tid for event in valid
                if event.kind in (VEHICLE_BEFORE, VEHICLE_AFTER)
            }),
            "camera_tids": sorted({event.producer_tid for event in cameras}),
            "sequence_gaps": sequence_gaps,
            "duplicate_sequences": duplicate_sequences,
            "clock_inversions": clock_inversions,
            "duplicate_before_frames": sorted(duplicate_before),
            "duplicate_after_frames": sorted(duplicate_after),
            "unknown_kind_events": len(ordered) - len(valid),
            "camera_burst_analysis": {
                "interframe_camera_count_distribution": {
                    str(key): value
                    for key, value in sorted(camera_count_distribution.items())
                },
                "complete_three_callback_groups": len(complete_bursts),
                "world_pattern_counts": dict(sorted(world_patterns.items())),
                "shape_pattern_counts": dict(sorted(shape_patterns.items())),
                "fov_pattern_counts": dict(sorted(fov_patterns.items())),
                "compared_burst_transitions": compared_burst_transitions,
                "pre_fov_equals_previous_final": pre_fov_equals_previous_final,
                "first_world_equals_previous_final_world": (
                    first_world_equals_previous_final),
                "shape_equals_previous_final_world": (
                    shape_equals_previous_final_world),
                "shape_previous_final_world_mismatch_frames": (
                    shape_mismatch_frames),
                "shape_equals_same_frame_final_world": (
                    shape_equals_same_frame_final_world),
                "same_slot_world_change_counts": same_slot_changes,
                "same_slot_world_comparisons": same_slot_comparisons,
                "vehicle_snapshot_by_slot": [
                    {
                        "slot": slot,
                        "snapshots": stats["snapshots"],
                        "pose_exact": stats["pose_exact"],
                        "linear_exact": stats["linear_exact"],
                        "both_exact": stats["both_exact"],
                        "changed": stats["changed"],
                    }
                    for slot, stats in enumerate(snapshot_slot_stats)
                ],
                "vehicle_snapshot_first_change_slot_counts": dict(
                    sorted(first_snapshot_change_slots.items())),
            },
        },
        "frames": frame_rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("events", type=pathlib.Path)
    parser.add_argument("--committed-events", type=int)
    parser.add_argument("--transaction-report", action="store_true")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.transaction_report and args.committed_events is not None:
        parser.error("--committed-events cannot be combined with --transaction-report")
    source_events = (read_transaction_events(args.events)
                     if args.transaction_report
                     else read_events(args.events, args.committed_events))
    report = analyze(source_events)
    rendered = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
