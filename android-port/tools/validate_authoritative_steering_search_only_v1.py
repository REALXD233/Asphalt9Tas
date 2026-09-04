#!/usr/bin/env python3
"""Strict validator for zero-write authoritative steering anchor search."""
from __future__ import annotations

import argparse
import pathlib
import struct
import sys

MAGIC = b"A9AST1\0\0"
BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
HEADER = struct.Struct("<8s4I20sI13Q15Q8I")
FRAME_SIZE = 172
SEARCH_FLAGS = 0x19F


def require(value: bool, message: str) -> None:
    if not value:
        raise ValueError(message)


def validate_blob(blob: bytes) -> dict[str, int]:
    require(len(blob) == HEADER.size, "search report must contain no frame audits")
    h = HEADER.unpack(blob)
    require((h[0], h[1], h[2], h[3], h[4]) ==
            (MAGIC, 1, HEADER.size, FRAME_SIZE, SEARCH_FLAGS), "header ABI/flags")
    require(h[5] == BUILD_ID and h[6] == 0, "build/reserved")
    require(all(h[index] != 0 for index in range(7, 20)), "zero identity/address")
    require(h[15] == h[14] + 4, "C9C is not C98+4")
    require(h[21] > 0 and h[41] < h[21], "search/matched cycle")
    require((h[23], h[24], h[25]) == (0, 0, 0), "runtime errors")
    require(all(h[index] == 0 for index in range(26, 34)), "gameplay write/action counters")
    require(h[36] == h[35] + h[34], "thread detach accounting")
    require(h[37] == 16667 and 1 <= h[38] <= 36000, "recording shape")
    require((h[39], h[40], h[42]) == (0, 0, 0), "search-only tail")
    return {"frames": h[38], "search_cycles": h[21],
            "rejected": h[22], "matched_cycle": h[41], "writes": 0}


def selftest_blob() -> bytes:
    addresses = (2680, 0x100000, 0x200000, 0x300000, 0x400000,
                 0x500000, 0x600000, 0x700000, 0x700004, 0x800000,
                 0x900000, 0xA00000, 0xB00000)
    counters = (999, 8, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)
    tail = (2, 4, 16667, 438, 0, 0, 6, 0)
    return HEADER.pack(MAGIC, 1, HEADER.size, FRAME_SIZE, SEARCH_FLAGS,
                       BUILD_ID, 0, *addresses, *counters, *tail)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    blob = selftest_blob() if args.selftest else pathlib.Path(args.report).read_bytes()
    result = validate_blob(blob)
    print("AUTHORITATIVE_SEARCH_ONLY_VALID passed=1", result)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error, TypeError) as error:
        print(f"AUTHORITATIVE_SEARCH_ONLY_VALID passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
