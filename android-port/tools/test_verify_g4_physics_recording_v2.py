#!/usr/bin/env python3
"""Positive and negative fixtures for verify_g4_physics_recording_v2.py."""

from __future__ import annotations

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile


HEADER = struct.Struct("<8sIIIIIIIIQII8s")
FRAME = struct.Struct("<QQfffIIB3s3f2f64s12sII")
INTERVAL = struct.Struct("<QII")


def fixture(skip: int, corrupt_physics: bool = False,
            malformed_interval: bool = False) -> bytes:
    interval_count = 3 if malformed_interval else 2
    current = skip == 0x48
    header = HEADER.pack(
        b"A9G4R2\0\0", 3 if current else 2, 64, 144, 16, 2,
        interval_count, 16667, 0x1F if current else 0x0F,
        1, 1, 0, bytes(8)
    )
    transform_values = [1.0, 0.0, 0.0, 0.0,
                        0.0, 1.0, 0.0, 0.0,
                        0.0, 0.0, 1.0, 0.0,
                        1.0, 2.0, 3.0, 1.0]
    if corrupt_physics:
        transform_values[0] = float("nan")
    transform = struct.pack("<16f", *transform_values)
    linear = struct.pack("<3f", 4.0, -2.0, 0.5)
    frames = b"".join(
        FRAME.pack(
            tick, tick * 16667000, 0.25 if tick else 0.0,
            -1.0 if tick else 0.0, 0.0, tick, skip, 0, bytes(3),
            1.0 if current and tick else 0.0, 0.0, 0.0,
            0.5 if current and tick else 0.0, 0.0,
            transform, linear, 0x7, 0
        )
        for tick in range(2)
    )
    interval_bits = struct.unpack("<I", struct.pack("<f", 1.0 / 60.0))[0]
    if malformed_interval:
        intervals = (INTERVAL.pack(0, 0, interval_bits) +
                     INTERVAL.pack(0, 2, interval_bits) +
                     INTERVAL.pack(1, 0, interval_bits))
    else:
        intervals = b"".join(INTERVAL.pack(tick, 0, interval_bits)
                             for tick in range(2))
    return header + frames + intervals


def run(verifier: pathlib.Path, path: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(verifier), str(path),
         "--expected-frames", "2"],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        good = root / "good.a9g4r2"
        current = root / "current.a9g4r2"
        bad_skip = root / "bad-skip.a9g4r2"
        bad_physics = root / "bad-physics.a9g4r2"
        bad_interval = root / "bad-interval.a9g4r2"
        good.write_bytes(fixture(0x78))
        current.write_bytes(fixture(0x48))
        bad_skip.write_bytes(fixture(0xF8))
        bad_physics.write_bytes(fixture(0x78, corrupt_physics=True))
        bad_interval.write_bytes(fixture(0x78, malformed_interval=True))
        positive = run(args.verifier, good)
        passed = (positive.returncode == 0 and
                  "unique_transform=1" in positive.stdout and
                  "unique_linear=1" in positive.stdout and
                  "first_transform_change=-1" in positive.stdout and
                  "first_linear_nonzero=0" in positive.stdout and
                  run(args.verifier, current).returncode == 0 and
                  run(args.verifier, bad_skip).returncode != 0 and
                  run(args.verifier, bad_physics).returncode != 0 and
                  run(args.verifier, bad_interval).returncode != 0)
    print(f"G5_PHYSICS_RECORDING_VERIFIER_SELFTEST passed={int(passed)} "
          "positive_legacy=1 positive_barrel=1 negative_skip_scope=1 negative_physics=1 "
          "negative_interval_ordinal=1 onset_diagnostics=1")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
