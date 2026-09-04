#!/usr/bin/env python3
"""Create the fixed one-frame Gate 10 brake-only A9UTK1 input."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from unified_tick_recording_v1 import (
    SKIP_BRAKE,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    if args.output.exists() or args.manifest.exists():
        raise SystemExit("output already exists")
    source_blob = args.source.read_bytes()
    fixed, frames = decode_recording(source_blob)
    if len(frames) != 1 or frames[0].skip_flags != 0xFF:
        raise SystemExit("source must be the one-frame all-skip Gate 6 packet")
    source = frames[0]
    target = UnifiedTickFrameV1(
        **{
            **source.__dict__,
            "brake": -1.0,
            "skip_flags": source.skip_flags & ~SKIP_BRAKE,
        }
    )
    output_blob = encode_recording([target], fixed_interval_us=fixed)
    args.output.write_bytes(output_blob)
    manifest = {
        "format": "A9UTK1_GATE10_BRAKE_ONLY_V1",
        "source_sha256": hashlib.sha256(source_blob).hexdigest(),
        "output_sha256": hashlib.sha256(output_blob).hexdigest(),
        "frames": 1,
        "fixed_interval_us": fixed,
        "brake_bits": "000080bf",
        "skip_flags": "0xfd",
        "enabled": ["brake_negative_longitudinal"],
        "disabled": ["steering", "accelerator", "nitro", "respawn", "barrel", "transform"],
    }
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"gate10_brake_input_sha256={manifest['output_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
