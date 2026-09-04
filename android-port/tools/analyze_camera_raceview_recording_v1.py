#!/usr/bin/env python3
"""Strict offline parser/analyzer for A9CRTR1 RaceView recordings."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import statistics
import struct


REPORT = struct.Struct("<8sIIII" + "Q" * 13 + "II" + "Q" * 5)
EVIDENCE = struct.Struct("<8sII" + "Q" * 8 + "IiQQQIIQ")
FRAME = struct.Struct("<IIQII" + "f" * 22)
REPORT_SIZE = REPORT.size + EVIDENCE.size
CONTINUOUS_PERMIT = (1 << 64) - 1


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def distance(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(left[:3], right[:3])))


def angle_degrees(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    lq, rq = left[3:7], right[3:7]
    ln = math.sqrt(sum(value * value for value in lq))
    rn = math.sqrt(sum(value * value for value in rq))
    if ln == 0.0 or rn == 0.0:
        return math.inf
    dot = abs(sum(a * b for a, b in zip(lq, rq)) / (ln * rn))
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, dot))))


def movement(transforms: list[tuple[float, ...]]) -> dict[str, float]:
    position_steps = [distance(a, b) for a, b in zip(transforms, transforms[1:])]
    rotation_steps = [angle_degrees(a, b) for a, b in zip(transforms, transforms[1:])]
    position_max_index = position_steps.index(max(position_steps)) + 1
    rotation_max_index = rotation_steps.index(max(rotation_steps)) + 1
    return {
        "changed_positions": sum(a[:3] != b[:3] for a, b in zip(transforms, transforms[1:])),
        "changed_rotations": sum(a[3:7] != b[3:7] for a, b in zip(transforms, transforms[1:])),
        "position_median": statistics.median(position_steps),
        "position_p95": percentile(position_steps, 0.95),
        "position_max": max(position_steps),
        "position_max_frame": position_max_index,
        "rotation_deg_median": statistics.median(rotation_steps),
        "rotation_deg_p95": percentile(rotation_steps, 0.95),
        "rotation_deg_max": max(rotation_steps),
        "rotation_max_frame": rotation_max_index,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("recording", type=pathlib.Path)
    args = parser.parse_args()
    data = args.recording.read_bytes()
    assert len(data) >= REPORT_SIZE
    header = REPORT.unpack_from(data, 0)
    (
        magic, version, report_size, action, report_flags,
        pid, start_ticks, game_base, payload_base, manager, shape, node,
        node_vptr, original_callback, wrapper, control, evidence_address,
        frames_address, requested_frames, captured_frames,
        payload_write_attempts, game_write_attempts, rollback_attempts,
        read_errors, semantic_errors,
    ) = header
    assert magic == b"A9CRTR1\0"
    assert version == 1 and report_size == REPORT_SIZE and action == 3
    assert len(data) == REPORT_SIZE + requested_frames * FRAME.size
    evidence = EVIDENCE.unpack_from(data, REPORT.size)
    (
        evidence_magic, evidence_version, evidence_size, wrapper_entries,
        original_calls, original_returns, idle_entries, claimed_permits,
        recorded_frames, failures, recursive_entries, processed_frames,
        last_status, last_manager, last_node, last_shape, last_tid,
        last_flags, evidence_reserved,
    ) = evidence
    assert evidence_magic == b"A9CRE1\0\0"
    assert evidence_version == 1 and evidence_size == EVIDENCE.size

    frames = []
    for index in range(requested_frames):
        raw = FRAME.unpack_from(data, REPORT_SIZE + index * FRAME.size)
        frame_index, tid, permit, flags, reserved, *floats = raw
        frames.append({
            "index": frame_index,
            "tid": tid,
            "permit": permit,
            "flags": flags,
            "reserved": reserved,
            "local": tuple(floats[0:7]),
            "world": tuple(floats[7:14]),
            "shape": tuple(floats[14:21]),
            "fov": floats[21],
        })

    all_floats = [value for frame in frames for key in ("local", "world", "shape") for value in frame[key]]
    all_floats += [frame["fov"] for frame in frames]
    indices_ok = [frame["index"] for frame in frames] == list(range(requested_frames))
    tids = sorted({frame["tid"] for frame in frames})
    permits_ok = all(frame["permit"] == CONTINUOUS_PERMIT for frame in frames)
    flags_ok = all(frame["flags"] == 0xF and frame["reserved"] == 0 for frame in frames)
    finite = all(math.isfinite(value) for value in all_floats)
    local = [frame["local"] for frame in frames]
    world = [frame["world"] for frame in frames]
    shapes = [frame["shape"] for frame in frames]
    fovs = [frame["fov"] for frame in frames]
    world_shape_distances = [distance(a, b) for a, b in zip(world, shapes)]
    world_shape_angles = [angle_degrees(a, b) for a, b in zip(world, shapes)]
    world_shape_position_max_frame = world_shape_distances.index(max(world_shape_distances))
    world_shape_rotation_max_frame = world_shape_angles.index(max(world_shape_angles))
    quaternion_norms = [
        math.sqrt(sum(value * value for value in transform[3:7]))
        for transforms in (local, world, shapes) for transform in transforms
    ]
    receipt_ok = (
        captured_frames == requested_frames == processed_frames == recorded_frames
        == claimed_permits
        and wrapper_entries == original_calls == original_returns
        and wrapper_entries >= requested_frames
        and failures == recursive_entries == read_errors == semantic_errors == 0
        and last_status == 2 and last_manager == manager and last_node == node
        and last_shape == shape and last_tid in tids and last_flags == 0xF
        and evidence_reserved == 0
    )
    result = {
        "format": "A9CRTR1",
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "report_flags": f"0x{report_flags:x}",
        "pid": pid,
        "start_ticks": start_ticks,
        "game_base": f"0x{game_base:x}",
        "payload_base": f"0x{payload_base:x}",
        "manager": f"0x{manager:x}",
        "shape": f"0x{shape:x}",
        "node": f"0x{node:x}",
        "node_vptr": f"0x{node_vptr:x}",
        "original_callback": f"0x{original_callback:x}",
        "wrapper": f"0x{wrapper:x}",
        "control": f"0x{control:x}",
        "evidence_address": f"0x{evidence_address:x}",
        "frames_address": f"0x{frames_address:x}",
        "requested_frames": requested_frames,
        "captured_frames": captured_frames,
        "wrapper_entries": wrapper_entries,
        "idle_entries": idle_entries,
        "producer_tids": tids,
        "payload_write_attempts": payload_write_attempts,
        "game_write_attempts_finalize": game_write_attempts,
        "rollback_attempts_finalize": rollback_attempts,
        "checks": {
            "receipt": receipt_ok,
            "indices": indices_ok,
            "continuous_permits": permits_ok,
            "frame_flags": flags_ok,
            "finite_values": finite,
        },
        "fov_radians": {
            "minimum": min(fovs),
            "maximum": max(fovs),
            "changed_steps": sum(a != b for a, b in zip(fovs, fovs[1:])),
        },
        "quaternion_norm": {
            "minimum": min(quaternion_norms),
            "maximum": max(quaternion_norms),
        },
        "local_movement": movement(local),
        "world_movement": movement(world),
        "shape_movement": movement(shapes),
        "world_to_shape": {
            "position_median": statistics.median(world_shape_distances),
            "position_p95": percentile(world_shape_distances, 0.95),
            "position_max": max(world_shape_distances),
            "position_max_frame": world_shape_position_max_frame,
            "rotation_deg_median": statistics.median(world_shape_angles),
            "rotation_deg_p95": percentile(world_shape_angles, 0.95),
            "rotation_deg_max": max(world_shape_angles),
            "rotation_max_frame": world_shape_rotation_max_frame,
        },
        "largest_transition_window": [
            {
                "frame": index,
                "world": world[index],
                "shape": shapes[index],
                "fov": fovs[index],
                "world_shape_position": world_shape_distances[index],
                "world_shape_rotation_deg": world_shape_angles[index],
            }
            for index in range(
                max(0, world_shape_position_max_frame - 2),
                min(requested_frames, world_shape_position_max_frame + 3),
            )
        ],
        "first": {
            "world": world[0], "shape": shapes[0], "fov": fovs[0],
        },
        "last": {
            "world": world[-1], "shape": shapes[-1], "fov": fovs[-1],
        },
    }
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if all(result["checks"].values()) else 2


if __name__ == "__main__":
    raise SystemExit(main())
