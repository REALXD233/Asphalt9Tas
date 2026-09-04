#!/usr/bin/env python3
"""Parse A9PST1 POST_PHYSICS vehicle-state traces."""

from __future__ import annotations

import argparse
import collections
import json
import math
import struct
from pathlib import Path


HEADER = struct.Struct("<8sIIII20sI" + "Q" * 14 + "q32s")
FRAME = struct.Struct("<QQqq" + "f" * 15 + "II")
MAGIC = b"A9PST1\0\0"


def stats(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "min": min(values) if values else None,
        "max": max(values) if values else None,
        "mean": sum(values) / len(values) if values else None,
    }


def read_trace(path: Path) -> tuple[dict[str, object], list[dict[str, object]]]:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("trace is shorter than header")
    raw = HEADER.unpack_from(data)
    (
        magic,
        version,
        header_size,
        frame_size,
        flags,
        build_id,
        _reserved0,
        pid,
        library_base,
        main_object,
        final_owner,
        physics_base,
        position_address,
        rotation_address,
        linear_address,
        angular_address,
        start_ns,
        target_frames,
        state_frames,
        state_read_errors,
        state_write_errors,
        fixed_interval_us,
        _reserved,
    ) = raw
    if magic != MAGIC or version != 1:
        raise ValueError(f"unsupported trace magic/version: {magic!r}/{version}")
    if header_size != HEADER.size or frame_size != FRAME.size:
        raise ValueError(
            f"ABI mismatch header={header_size}/{HEADER.size} "
            f"frame={frame_size}/{FRAME.size}"
        )
    expected = header_size + state_frames * frame_size
    if len(data) != expected:
        raise ValueError(f"size mismatch file={len(data)} expected={expected}")

    frames: list[dict[str, object]] = []
    for index in range(state_frames):
        values = FRAME.unpack_from(data, header_size + index * frame_size)
        tick, monotonic_ns, original_us, applied_us = values[:4]
        floats = values[4:19]
        frame_flags, _frame_reserved = values[19:21]
        frame = {
            "tick": tick,
            "relative_ms": (monotonic_ns - start_ns) / 1_000_000.0,
            "original_interval_us": original_us,
            "applied_interval_us": applied_us,
            "position": list(floats[0:3]),
            "rotation": list(floats[3:7]),
            "linear": list(floats[7:10]),
            "angular": list(floats[10:13]),
            "c98": floats[13],
            "c9c": floats[14],
            "flags": frame_flags,
        }
        frames.append(frame)

    header = {
        "trace": str(path),
        "clean": bool(flags & 1),
        "owner_change_gate": bool(flags & 2),
        "state_complete_gate": bool(flags & 4),
        "build_id": build_id.hex(),
        "pid": pid,
        "library_base": f"0x{library_base:x}",
        "main_object": f"0x{main_object:x}",
        "final_owner": f"0x{final_owner:x}",
        "physics_base": f"0x{physics_base:x}",
        "position_address": f"0x{position_address:x}",
        "rotation_address": f"0x{rotation_address:x}",
        "linear_address": f"0x{linear_address:x}",
        "angular_address": f"0x{angular_address:x}",
        "target_frames": target_frames,
        "state_frames": state_frames,
        "state_read_errors": state_read_errors,
        "state_write_errors": state_write_errors,
        "fixed_interval_us": fixed_interval_us,
    }
    return header, frames


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--frames", action="store_true")
    args = parser.parse_args()

    header, frames = read_trace(args.trace)
    ticks = [int(frame["tick"]) for frame in frames]
    expected_ticks = list(range(len(frames)))
    applied = collections.Counter(
        int(frame["applied_interval_us"]) for frame in frames
    )
    original = [float(frame["original_interval_us"]) for frame in frames]
    intervals = [
        float(right["relative_ms"]) - float(left["relative_ms"])
        for left, right in zip(frames, frames[1:])
    ]
    quaternion_norms = [
        math.sqrt(sum(float(value) ** 2 for value in frame["rotation"]))
        for frame in frames
    ]
    all_finite = all(
        math.isfinite(float(value))
        for frame in frames
        for key in ("position", "rotation", "linear", "angular")
        for value in frame[key]
    )
    summary: dict[str, object] = {
        **header,
        "ticks_sequential": ticks == expected_ticks,
        "all_state_finite": all_finite,
        "all_frame_flags_valid": all(int(frame["flags"]) == 1 for frame in frames),
        "original_interval_us": stats(original),
        "applied_interval_histogram": {
            str(value): count for value, count in applied.most_common()
        },
        "post_state_interval_ms": stats(intervals),
        "quaternion_norm": stats(quaternion_norms),
        "first_state": frames[0] if frames else None,
        "last_state": frames[-1] if frames else None,
    }
    if args.frames:
        summary["frames"] = frames
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
