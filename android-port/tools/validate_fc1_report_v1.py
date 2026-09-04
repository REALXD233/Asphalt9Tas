#!/usr/bin/env python3
"""Strict validator for the packed 336-byte FC-1 report."""

from __future__ import annotations

import argparse
import pathlib
import struct


REPORT_SIZE = 336
REQUIRED_FLAGS = 0x7FF
DETACHED_PHASE = 7


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def validate(data: bytes, expected_pid: int, expected_base: int) -> None:
    if len(data) != REPORT_SIZE:
        raise ValueError("FC-1 report size mismatch")
    if data[:8] != b"A9FC1R1\0" or u32(data, 8) != 1 or u32(data, 12) != REPORT_SIZE:
        raise ValueError("FC-1 report header mismatch")
    if u32(data, 16) != REQUIRED_FLAGS or u32(data, 20) != DETACHED_PHASE:
        raise ValueError("FC-1 report did not finish with every required flag")
    if u64(data, 24) != expected_pid or u64(data, 32) != expected_base:
        raise ValueError("FC-1 report target identity mismatch")
    context, callback_list, callback_flags = (
        u64(data, 40), u64(data, 48), u64(data, 56)
    )
    car, original_vptr, shadow_vptr = (
        u64(data, 64), u64(data, 72), u64(data, 80)
    )
    payload_shadow, payload_control, payload_evidence, wrapper = (
        u64(data, 88), u64(data, 96), u64(data, 104), u64(data, 112)
    )
    identities = (
        context, callback_list, callback_flags, car, original_vptr,
        shadow_vptr, payload_shadow, payload_control, payload_evidence, wrapper,
    )
    if any(value == 0 for value in identities):
        raise ValueError("FC-1 report contains a null pinned identity")
    if (callback_list != context + 0x180 or
            callback_flags != context + 0x1A0 or
            original_vptr != expected_base + 0x7EE8D18 or
            shadow_vptr != payload_shadow + 0x58):
        raise ValueError("FC-1 report pinned-identity relationship mismatch")
    owner_tid = i32(data, 120)
    initial_threads, final_threads = u32(data, 124), u32(data, 128)
    if u32(data, 132) != 0:
        raise ValueError("FC-1 success report contains an open rejection reason")
    if owner_tid <= 0 or initial_threads == 0 or final_threads < initial_threads:
        raise ValueError("FC-1 report thread accounting mismatch")
    open_ns, close_ns = u64(data, 136), u64(data, 144)
    if open_ns == 0 or close_ns < open_ns:
        raise ValueError("FC-1 report frame boundary mismatch")
    counters = tuple(u64(data, offset) for offset in range(152, 208, 8))
    if counters != (1, 0, 0, 0, 0, 0, 0):
        raise ValueError("FC-1 report write/error counters mismatch")

    evidence = 208
    if (data[evidence:evidence + 8] != b"A9FC0E1\0" or
            u32(data, evidence + 8) != 1 or
            u32(data, evidence + 12) != 128):
        raise ValueError("FC-1 payload evidence header mismatch")
    evidence_counts = tuple(
        u64(data, evidence + offset) for offset in (16, 24, 32, 40, 48)
    )
    if evidence_counts != (1, 1, 1, 0, 0):
        raise ValueError("FC-1 payload evidence counters mismatch")
    if (u64(data, evidence + 56) != car or
            u64(data, evidence + 64) == 0 or
            u64(data, evidence + 72) != shadow_vptr or
            u64(data, evidence + 80) != original_vptr or
            i32(data, evidence + 96) != 0):
        raise ValueError("FC-1 payload evidence identity mismatch")


def selftest() -> None:
    data = bytearray(REPORT_SIZE)
    data[:8] = b"A9FC1R1\0"
    struct.pack_into("<IIII", data, 8, 1, REPORT_SIZE, REQUIRED_FLAGS,
                     DETACHED_PHASE)
    struct.pack_into("<QQ", data, 24, 1234, 0x70000000)
    context = 0x71000000
    payload_shadow = 0x73000000
    original_vptr = 0x70000000 + 0x7EE8D18
    shadow_vptr = payload_shadow + 0x58
    struct.pack_into("<QQQ", data, 40, context, context + 0x180,
                     context + 0x1A0)
    struct.pack_into("<QQQ", data, 64, 0x72000000, original_vptr, shadow_vptr)
    struct.pack_into("<QQQQ", data, 88, payload_shadow, 0x73001000,
                     0x73002000, 0x74000000)
    struct.pack_into("<iII", data, 120, 1240, 20, 20)
    struct.pack_into("<QQ", data, 136, 100, 200)
    struct.pack_into("<QQQQQQQ", data, 152, 1, 0, 0, 0, 0, 0, 0)
    evidence = 208
    data[evidence:evidence + 8] = b"A9FC0E1\0"
    struct.pack_into("<II", data, evidence + 8, 1, 128)
    struct.pack_into("<QQQQQ", data, evidence + 16, 1, 1, 1, 0, 0)
    struct.pack_into("<QQQQ", data, evidence + 56, 0x72000000, 0x74000000,
                     shadow_vptr, original_vptr)
    struct.pack_into("<i", data, evidence + 96, 0)
    validate(bytes(data), 1234, 0x70000000)
    corruptions = (
        (16, "<I", 0),
        (152, "<Q", 2),
        (evidence + 24, "<Q", 0),
        (48, "<Q", context + 0x188),
        (72, "<Q", original_vptr + 8),
        (80, "<Q", shadow_vptr + 8),
        (132, "<I", 1),
    )
    for offset, layout, value in corruptions:
        broken = bytearray(data)
        struct.pack_into(layout, broken, offset, value)
        try:
            validate(bytes(broken), 1234, 0x70000000)
        except ValueError:
            pass
        else:
            raise AssertionError(f"corrupt FC-1 report accepted at offset {offset}")
    try:
        validate(bytes(data[:-1]), 1234, 0x70000000)
    except ValueError:
        pass
    else:
        raise AssertionError("short FC-1 report accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--base", type=lambda value: int(value, 0))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        print("FC1_REPORT_VALIDATOR passed=1 size=336 flags=0x7ff evidence=1 negative=8")
        return 0
    if args.report is None or args.pid is None or args.base is None:
        parser.error("report, --pid and --base are required")
    validate(args.report.read_bytes(), args.pid, args.base)
    print(f"FC1_REPORT_VALID passed=1 pid={args.pid} base=0x{args.base:x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
