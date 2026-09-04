#!/usr/bin/env python3
"""Positive and negative fixtures for verify_g4_action_recording_v1.py."""

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


def fixture(skip: int) -> bytes:
    header = HEADER.pack(
        b"A9G4R1\0\0", 1, 64, 144, 16, 2, 2, 16667, 0x0F,
        1, 1, 0, bytes(8)
    )
    frames = b"".join(
        FRAME.pack(
            tick, tick * 16667000, 0.25 if tick else 0.0,
            -1.0 if tick else 0.0, 0.0, tick, skip, 0, bytes(3),
            0.0, 0.0, 0.0, 0.0, 0.0, bytes(64), bytes(12), 0x3, 0
        )
        for tick in range(2)
    )
    interval_bits = struct.unpack("<I", struct.pack("<f", 1.0 / 60.0))[0]
    intervals = b"".join(INTERVAL.pack(tick, 0, interval_bits)
                         for tick in range(2))
    return header + frames + intervals


def run(verifier: pathlib.Path, path: pathlib.Path) -> int:
    return subprocess.run(
        [sys.executable, "-B", str(verifier), str(path),
         "--expected-frames", "2"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        good = root / "good.a9g4r1"
        bad = root / "bad.a9g4r1"
        good.write_bytes(fixture(0xF8))
        bad.write_bytes(fixture(0xF0))
        passed = run(args.verifier, good) == 0 and run(args.verifier, bad) != 0
    print(f"G4_ACTION_RECORDING_VERIFIER_SELFTEST passed={int(passed)} "
          "positive=1 negative_skip_scope=1")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
