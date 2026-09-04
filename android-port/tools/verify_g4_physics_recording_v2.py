#!/usr/bin/env python3
"""Strict verifier for a G5-complete G4R2 action + 64+12 physics bundle."""

from __future__ import annotations

import argparse
import math
import pathlib
import struct
import sys


HEADER = struct.Struct("<8sIIIIIIIIQII8s")
FRAME = struct.Struct("<QQfffIIB3s3f2f64s12sII")
INTERVAL = struct.Struct("<QII")
MAGIC = b"A9G4R2\0\0"
LEGACY_VERSION = 2
CURRENT_VERSION = 3
LEGACY_FLAGS = 0x0F
CURRENT_FLAGS = 0x1F
LEGACY_SKIP = 0x78
CURRENT_SKIP = 0x48


def fail(message: str) -> int:
    print(f"G5_PHYSICS_RECORDING_VERIFY passed=0 error={message}", file=sys.stderr)
    return 1


def finite_raw_floats(raw: bytes, count: int) -> bool:
    values = struct.unpack(f"<{count}f", raw)
    return all(math.isfinite(value) and abs(value) <= 1_000_000.0
               for value in values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("recording", type=pathlib.Path)
    parser.add_argument("--expected-frames", type=int, required=True)
    args = parser.parse_args()
    data = args.recording.read_bytes()
    if len(data) < HEADER.size:
        return fail("short_header")
    fields = HEADER.unpack_from(data)
    (magic, version, header_size, frame_size, interval_size, frame_count,
     interval_count, fixed_delta_us, flags, session_id, generation, reserved0,
     reserved) = fields
    legacy = version == LEGACY_VERSION and flags == LEGACY_FLAGS
    current = version == CURRENT_VERSION and flags == CURRENT_FLAGS
    if (magic != MAGIC or not (legacy or current) or
            header_size != HEADER.size or
            frame_size != FRAME.size or interval_size != INTERVAL.size or
            frame_count != args.expected_frames or interval_count == 0 or
            not 1000 <= fixed_delta_us <= 100000 or
            session_id == 0 or generation == 0 or reserved0 != 0 or
            reserved != bytes(8)):
        return fail("header_identity")
    expected_size = (HEADER.size + frame_count * FRAME.size +
                     interval_count * INTERVAL.size)
    if len(data) != expected_size:
        return fail("exact_size")

    offset = HEADER.size
    nitro_total = 0
    first_transform: bytes | None = None
    first_transform_change = -1
    first_linear_nonzero = -1
    unique_transforms: set[bytes] = set()
    unique_linear: set[bytes] = set()
    barrel_frames = 0
    for index in range(frame_count):
        frame = FRAME.unpack_from(data, offset)
        offset += FRAME.size
        (tick, monotonic_ns, steering, brake, accelerator, nitro, skip,
         respawn, padding, ax, ay, az, rbx0, rbx1, transform, linear,
         frame_flags, frame_reserved) = frame
        if (tick != index or monotonic_ns != index * fixed_delta_us * 1000 or
                not math.isfinite(steering) or abs(steering) > 1.0 or
                not math.isfinite(brake) or abs(brake) > 1.05 or
                accelerator != 0.0 or nitro > 2 or
                skip != (LEGACY_SKIP if legacy else CURRENT_SKIP) or
                respawn != 0 or padding != bytes(3) or
                (legacy and any(value != 0.0
                                for value in (ax, ay, az, rbx0, rbx1))) or
                (current and any(not math.isfinite(value) or
                                 abs(value) > 1_000_000.0
                                 for value in (ax, ay, az, rbx0, rbx1))) or
                not finite_raw_floats(transform, 16) or
                not finite_raw_floats(linear, 3) or
                frame_flags != 0x7 or frame_reserved != 0):
            return fail(f"frame_{index}")
        nitro_total += nitro
        barrel_frames += int(any(value != 0.0
                                 for value in (ax, ay, az, rbx0, rbx1)))
        if first_transform is None:
            first_transform = transform
        elif first_transform_change < 0 and transform != first_transform:
            first_transform_change = index
        if (first_linear_nonzero < 0 and
                any(value != 0.0 for value in struct.unpack("<3f", linear))):
            first_linear_nonzero = index
        unique_transforms.add(transform)
        unique_linear.add(linear)

    last_tick = -1
    next_ordinal = 0
    calls_per_tick = [0] * frame_count
    for index in range(interval_count):
        tick, ordinal, output_bits = INTERVAL.unpack_from(data, offset)
        offset += INTERVAL.size
        value = struct.unpack("<f", struct.pack("<I", output_bits))[0]
        if tick >= frame_count or not math.isfinite(value) or not 0.001 <= value <= 0.1:
            return fail(f"interval_value_{index}")
        if tick != last_tick:
            if tick <= last_tick:
                return fail(f"interval_order_{index}")
            last_tick = tick
            next_ordinal = 0
        if ordinal != next_ordinal:
            return fail(f"interval_ordinal_{index}")
        next_ordinal += 1
        calls_per_tick[tick] += 1
    if any(count == 0 for count in calls_per_tick):
        return fail("missing_tick_interval")

    print(
        "G5_PHYSICS_RECORDING_VERIFY passed=1 "
        f"frames={frame_count} intervals={interval_count} nitro={nitro_total} "
        "brake=1 steering=1 physics_valid=1 "
        f"unique_transform={len(unique_transforms)} "
        f"unique_linear={len(unique_linear)} "
        f"first_transform_change={first_transform_change} "
        f"first_linear_nonzero={first_linear_nonzero} "
        f"barrel_scope={'legacy_skipped' if legacy else 'captured'} "
        f"barrel_nonzero_frames={barrel_frames} "
        "interval_semantics=alu_default_passthrough_diagnostic"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
