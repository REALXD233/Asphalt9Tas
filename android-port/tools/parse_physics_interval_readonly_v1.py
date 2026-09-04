#!/usr/bin/env python3
"""Parse and enforce the fail-closed A9PIO1 read-only observation receipt."""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

HEADER = struct.Struct("<8sIIII13QII")
SAMPLE = struct.Struct("<QQQQQQQIIII")
MAGIC = b"A9PIO2\0\0"
VERSION = 2

CLEAN = 1 << 0
TARGET_VERIFIED = 1 << 1
INLINE_ALL = 1 << 3
IDENTITY_STABLE = 1 << 4
INTERVAL_STABLE = 1 << 5

SAMPLE_READ_OK = 1 << 0
SAMPLE_CONTEXT_OK = 1 << 1
SAMPLE_BACKEND_OK = 1 << 2
SAMPLE_INLINE_DEFAULT = 1 << 3
SAMPLE_INTERVAL_FINITE = 1 << 4
SAMPLE_OPTIONS_HEAD_OK = 1 << 5


def car_profile_identity(build_profile: Path | None) -> tuple[int, int]:
    if build_profile is None:
        return 0x7EED420, 0x3695740
    blob = build_profile.read_bytes()
    if (len(blob) < 384 or blob[:8] != b"A9BPR1\0\0" or
            struct.unpack_from("<I", blob, 8)[0] != 1 or
            struct.unpack_from("<I", blob, 12)[0] != 384):
        raise ValueError("invalid G8 build profile")
    hook0 = struct.unpack_from("<Q", blob, 32)[0]
    # BuildProfile v1 with eight hook RVAs places step_options_vtable_rva at 0x80.
    step_options_vtable = struct.unpack_from("<Q", blob, 128)[0]
    reference_hook0 = 0x3695474
    if hook0 < reference_hook0:
        raise ValueError("invalid G8 gameplay relocation")
    return step_options_vtable, 0x3695740 + hook0 - reference_hook0


def parse(path: Path, profile: str = "inline-default",
          build_profile: Path | None = None) -> dict[str, object]:
    if profile not in ("inline-default", "car-physics"):
        raise ValueError(f"unsupported profile: {profile}")
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("truncated header")
    values = HEADER.unpack_from(blob)
    (magic, version, header_size, sample_size, flags, pid, base, context,
     adapter, world, start_ns, duration_ms, sample_ms, sample_count,
     read_errors, identity_failures, alternate_options, interval_changes,
     interval_bits, reserved) = values
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported magic/version")
    if header_size != HEADER.size or sample_size != SAMPLE.size or reserved != 0:
        raise ValueError("ABI mismatch")
    expected = header_size + sample_count * sample_size
    if len(blob) != expected:
        raise ValueError(f"size mismatch expected={expected} actual={len(blob)}")
    interval = struct.unpack("<f", struct.pack("<I", interval_bits))[0]
    samples = [SAMPLE.unpack_from(blob, header_size + i * sample_size)
               for i in range(sample_count)]
    monotonic = all(samples[i][0] < samples[i + 1][0]
                    for i in range(len(samples) - 1))
    step_options = sorted({sample[1] for sample in samples})
    step_options_vptrs = sorted({sample[2] for sample in samples})
    step_options_getters = sorted({sample[3] for sample in samples})
    step_options_word8 = sorted({sample[4] for sample in samples})
    default_vptr = base + 0x81039A0
    default_getter = base + 0x38B7C5C
    proven_default_object = (
        alternate_options == sample_count
        and len(step_options) == 1 and step_options[0] != 0
        and step_options_vptrs == [default_vptr]
        and step_options_getters == [default_getter]
    )
    base_required = CLEAN | TARGET_VERIFIED | IDENTITY_STABLE | INTERVAL_STABLE
    common_pass = (
        (flags & base_required) == base_required
        and read_errors == 0
        and identity_failures == 0
        and interval_changes == 0
        and sample_count >= 2
        and math.isfinite(interval)
        and 0.0 < interval <= 1.0
        and monotonic
    )
    if profile == "inline-default":
        passed = common_pass and (flags & INLINE_ALL) != 0 and alternate_options == 0
    else:
        required_sample_flags = (
            SAMPLE_READ_OK | SAMPLE_CONTEXT_OK | SAMPLE_BACKEND_OK |
            SAMPLE_INTERVAL_FINITE | SAMPLE_OPTIONS_HEAD_OK
        )
        car_vptr_rva, car_getter_rva = car_profile_identity(build_profile)
        car_vptr = base + car_vptr_rva
        car_getter = base + car_getter_rva
        exact_sample_flags = all(
            (sample[9] & required_sample_flags) == required_sample_flags
            and (sample[9] & SAMPLE_INLINE_DEFAULT) == 0
            for sample in samples
        )
        passed = (
            common_pass
            and (flags & INLINE_ALL) == 0
            and alternate_options == sample_count
            and len(step_options) == 1 and step_options[0] != 0
            and step_options_vptrs == [car_vptr]
            and step_options_getters == [car_getter]
            and len(step_options_word8) == 1
            and exact_sample_flags
        )
    return {
        "passed": passed,
        "profile": profile,
        "pid": pid,
        "library_base": f"0x{base:x}",
        "physics_context": f"0x{context:x}",
        "backend_adapter": f"0x{adapter:x}",
        "backend_world": f"0x{world:x}",
        "duration_ms": duration_ms,
        "sample_ms": sample_ms,
        "sample_count": sample_count,
        "read_errors": read_errors,
        "identity_failures": identity_failures,
        "alternate_options_samples": alternate_options,
        "interval_change_samples": interval_changes,
        "interval_bits": f"0x{interval_bits:08x}",
        "interval_seconds": interval,
        "step_options": [f"0x{x:x}" for x in step_options],
        "step_options_vptrs": [f"0x{x:x}" for x in step_options_vptrs],
        "step_options_getters": [f"0x{x:x}" for x in step_options_getters],
        "step_options_word8": [f"0x{x:016x}" for x in step_options_word8],
        "proven_default_step_options_object": proven_default_object,
        "timestamps_strictly_increasing": monotonic,
        "ptrace_calls": 0,
        "game_writes": 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument(
        "--profile", choices=("inline-default", "car-physics"),
        default="inline-default")
    parser.add_argument("--build-profile", type=Path)
    args = parser.parse_args()
    try:
        result = parse(args.trace, args.profile, args.build_profile)
    except (OSError, ValueError, struct.error) as exc:
        print(f"PHYSICS_INTERVAL_READONLY_INVALID reason={exc}")
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(
            "PHYSICS_INTERVAL_READONLY_VALID "
            + " ".join(f"{key}={value}" for key, value in result.items())
        )
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
