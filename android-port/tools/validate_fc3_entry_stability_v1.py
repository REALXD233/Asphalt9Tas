#!/usr/bin/env python3
"""Strict validator for the attach/read/detach-only A9FC3E1 report."""

from __future__ import annotations

import argparse
import pathlib
import struct


REPORT_SIZE = 104
REQUIRED_FLAGS = 0xFF


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def validate(data: bytes) -> dict[str, int]:
    require(len(data) == REPORT_SIZE, "report size")
    require(data[:8] == b"A9FC3E1\0", "report magic")
    require(u32(data, 8) == 1 and u32(data, 12) == REPORT_SIZE,
            "report version/declared size")
    require(u32(data, 16) == REQUIRED_FLAGS, "required flags")
    require(u32(data, 20) == 0, "cleanup disposition")
    pid = u64(data, 24)
    start_time = u64(data, 32)
    attach_start = u64(data, 40)
    attach_end = u64(data, 48)
    require(pid > 0 and start_time > 0, "process identity")
    require(attach_start > 0 and attach_end > attach_start,
            "attach timeline")
    initial = u32(data, 56)
    final = u32(data, 60)
    detached = u32(data, 64)
    retired = u32(data, 68)
    require(initial > 0 and final == initial and detached == initial,
            "thread accounting")
    require(retired == 0, "entry-only stopped snapshot cannot retire tasks")
    require([u64(data, offset) for offset in (72, 80, 88, 96)] == [0] * 4,
            "error/write counters")
    return {
        "pid": pid,
        "start_time": start_time,
        "initial_threads": initial,
        "attach_duration_ns": attach_end - attach_start,
    }


def make_valid() -> bytes:
    data = bytearray(REPORT_SIZE)
    data[:8] = b"A9FC3E1\0"
    struct.pack_into("<IIII", data, 8, 1, REPORT_SIZE, REQUIRED_FLAGS, 0)
    struct.pack_into("<QQQQ", data, 24, 1234, 5678, 100, 200)
    struct.pack_into("<IIII", data, 56, 282, 282, 282, 0)
    return bytes(data)


def selftest() -> None:
    valid = make_valid()
    parsed = validate(valid)
    require(parsed["initial_threads"] == 282, "positive selftest")
    negatives = 0
    for offset, value in ((16, 0x7F), (20, 1), (60, 281),
                          (64, 281), (68, 1), (88, 1), (96, 1)):
        bad = bytearray(valid)
        if offset in (16, 20, 60, 64, 68):
            struct.pack_into("<I", bad, offset, value)
        else:
            struct.pack_into("<Q", bad, offset, value)
        try:
            validate(bytes(bad))
        except ValueError:
            negatives += 1
    require(negatives == 7, "negative selftest coverage")
    print("FC3_ENTRY_STABILITY_VALIDATOR_SELFTEST passed=1 size=104 "
          "negative=7 debug_writes=0 game_writes=0")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=pathlib.Path, nargs="?")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return 0
    if args.report is None:
        parser.error("report is required unless --selftest is used")
    result = validate(args.report.read_bytes())
    print("FC3_ENTRY_STABILITY_REPORT_OK "
          f"pid={result['pid']} initial={result['initial_threads']} "
          f"attach_ns={result['attach_duration_ns']} debug_writes=0 game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
