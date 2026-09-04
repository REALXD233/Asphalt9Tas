#!/usr/bin/env python3
"""Strict validator for the packed A9FC3P1 phase-map report."""

from __future__ import annotations

import argparse
import math
import pathlib
import struct

import validate_fc2_report_v1 as fc2_validator


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc3_phase_map_v1.h"
REPORT_SIZE = 408
REQUIRED_FLAGS = 0xFFFFF
PHASE_DETACHED = 13
TIME_OFFSETS = tuple(range(136, 224, 8))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def verify_header_contract() -> None:
    text = HEADER.read_text(encoding="utf-8")
    for token in (
        "kWaitingBootstrapClose = 1",
        "kWaitingDelta = 2",
        "kWaitingC98 = 3",
        "kWaitingCallbackOpen = 4",
        "kWaitingC9C = 5",
        "kWaitingF64 = 6",
        "kWaitingDedicated = 7",
        "kWaitingCallbackClose = 8",
        "kWaitingWorldCommit = 9",
        "kWaitingNextDelta = 10",
        "kWaitingNextC98 = 11",
        "kProved = 12",
        "kDetached = 13",
        "static_assert(sizeof(Report) == 408",
        "offsetof(Report, pid) == 32",
        "offsetof(Report, bootstrap_close_ns) == 136",
        "offsetof(Report, callback_flag_hits) == 260",
        "offsetof(Report, cleanup_disposition) == 380",
        "offsetof(Report, f64_bits) == 384",
        "offsetof(Report, world_commit_bits) == 388",
        "offsetof(Report, pre_anchor_delta_hits) == 392",
        "kRequiredFinalFlags",
    ):
        require(token in text, f"phase-map ABI token missing: {token}")


def validate(data: bytes) -> dict[str, int]:
    require(len(data) == REPORT_SIZE, "report size")
    require(data[:8] == b"A9FC3P1\0", "report magic")
    require(u32(data, 8) == 1 and u32(data, 12) == REPORT_SIZE,
            "report version/declared size")
    require(u32(data, 16) == REQUIRED_FLAGS, "required flags")
    require(u32(data, 20) == 0, "reject reasons")
    require(u32(data, 24) == PHASE_DETACHED, "final phase")
    owner_tid = i32(data, 28)
    require(owner_tid > 0, "callback owner")

    identities = [u64(data, 32 + 8 * index) for index in range(13)]
    (pid, base, main_owner, final_owner, delta, c98, c9c,
     callback_flags, callback_list, car, dedicated, f64, world_commit) = identities
    require(all(identities), "zero identity/address")
    require(delta == main_owner + 0x150, "fixed-delta relation")
    require(c98 == final_owner + 0xC98 and c9c == c98 + 4,
            "final-control relation")
    require(callback_flags == callback_list + 0x20,
            "callback-list relation")
    require(f64 == car + 0xF64, "F64 relation")
    require(delta % 8 == 0 and c98 % 4 == 0 and
            callback_flags % 2 == 0 and f64 % 4 == 0 and
            world_commit % 4 == 0, "watchpoint alignment")

    times = [u64(data, offset) for offset in TIME_OFFSETS]
    require(all(left < right for left, right in zip(times, times[1:])),
            "phase timestamp order")
    tids = [i32(data, 224 + 4 * index) for index in range(7)]
    require(all(tid > 0 for tid in tids), "phase event tid")
    require(tids[3] == owner_tid, "F64 callback-owner tid")
    require(tids[4] != owner_tid, "world commit must use backend tid")
    car_index = u32(data, 252)
    dedicated_index = u32(data, 256)
    require(car_index < dedicated_index and dedicated_index != 0xFFFFFFFF,
            "callback ordering")

    (callback_hits, delta_hits, c98_hits, c9c_hits, f64_hits,
     dedicated_hits, world_commit_hits) = (
        u64(data, 260 + 8 * index) for index in range(7)
    )
    require(callback_hits >= 4 and delta_hits == 2 and c98_hits == 2 and
            c9c_hits == 1 and f64_hits == 1 and dedicated_hits == 1 and
            world_commit_hits == 1, "event counters")
    require([u64(data, 316 + 8 * index) for index in range(7)] == [0] * 7,
            "error/write/call counters")
    initial_threads = u32(data, 372)
    final_threads = u32(data, 376)
    require(initial_threads > 0 and final_threads == initial_threads,
            "thread accounting")
    require(u32(data, 380) == 0, "cleanup disposition")
    f64_value = struct.unpack_from("<f", data, 384)[0]
    world_commit_value = struct.unpack_from("<f", data, 388)[0]
    require(math.isfinite(f64_value) and abs(f64_value) <= 1_000_000.0,
            "F64 value")
    require(math.isfinite(world_commit_value) and abs(world_commit_value) <= 60.0,
            "world commit value")
    pre_anchor_delta_hits = u64(data, 392)
    pre_anchor_c98_hits = u64(data, 400)
    require(pre_anchor_delta_hits <= 16 and pre_anchor_c98_hits <= 16,
            "pre-anchor event counters")
    return {
        "pid": pid,
        "base": base,
        "callback_flags": callback_flags,
        "callback_list": callback_list,
        "car": car,
        "dedicated": dedicated,
        "owner_tid": owner_tid,
        "initial_threads": initial_threads,
        "bootstrap_close_ns": times[0],
        "delta_ns": times[1],
        "c98_ns": times[2],
        "callback_open_ns": times[3],
        "c9c_ns": times[4],
        "f64_ns": times[5],
        "dedicated_ns": times[6],
        "callback_close_ns": times[7],
        "world_commit_ns": times[8],
        "next_delta_ns": times[9],
        "next_c98_ns": times[10],
    }


def cross_validate_fc2(phase: dict[str, int], fc2_data: bytes) -> None:
    fc2_validator.validate(fc2_data)
    require(phase["pid"] == u64(fc2_data, 24) and
            phase["base"] == u64(fc2_data, 32) and
            phase["callback_list"] == u64(fc2_data, 48) and
            phase["callback_flags"] == u64(fc2_data, 56) and
            phase["car"] == u64(fc2_data, 64) and
            phase["dedicated"] == u64(fc2_data, 128) and
            phase["owner_tid"] == i32(fc2_data, 144) and
            phase["initial_threads"] == u32(fc2_data, 148),
            "FC-2/phase-map identity mismatch")
    bootstrap, registration, removal, compaction = (
        u64(fc2_data, 160 + 8 * index) for index in range(4)
    )
    ordered = (
        bootstrap,
        phase["bootstrap_close_ns"],
        phase["delta_ns"],
        phase["c98_ns"],
        phase["callback_open_ns"],
        registration,
        phase["c9c_ns"],
        phase["f64_ns"],
        phase["dedicated_ns"],
        phase["callback_close_ns"],
        removal,
        phase["world_commit_ns"],
        phase["next_delta_ns"],
        phase["next_c98_ns"],
        compaction,
    )
    require(all(left < right for left, right in zip(ordered, ordered[1:])),
            "FC-2/phase-map nested timeline")


def make_valid_report() -> bytes:
    data = bytearray(REPORT_SIZE)
    data[:8] = b"A9FC3P1\0"
    struct.pack_into("<IIIIIi", data, 8, 1, REPORT_SIZE, REQUIRED_FLAGS,
                     0, PHASE_DETACHED, 1300)
    base = 0x700000000000
    main = 0x710000001000
    final = 0x720000002000
    callback_list = 0x730000003180
    identities = (
        1234, base, main, final, main + 0x150, final + 0xC98,
        final + 0xC9C, callback_list + 0x20, callback_list,
        0x740000004000, 0x750000005000,
        0x740000004000 + 0xF64, 0x760000006188,
    )
    struct.pack_into("<13Q", data, 32, *identities)
    struct.pack_into("<11Q", data, 136, *range(20, 130, 10))
    struct.pack_into("<7i", data, 224, 1301, 1302, 1303, 1300, 1301, 1302, 1303)
    struct.pack_into("<II", data, 252, 4, 5)
    struct.pack_into("<7Q", data, 260, 6, 2, 2, 1, 1, 1, 1)
    struct.pack_into("<7Q", data, 316, *([0] * 7))
    struct.pack_into("<IIIffQQ", data, 372, 200, 200, 0, 42.0, 0.016, 1, 1)
    return bytes(data)


def selftest() -> None:
    verify_header_contract()
    valid = make_valid_report()
    validate(valid)
    for offset, fmt, value in (
        (16, "I", 0),
        (152, "Q", 19),
        (256, "I", 4),
        (268, "Q", 3),
        (340, "Q", 1),
        (380, "I", 1),
        (388, "I", 0x7FC00000),
        (392, "Q", 17),
    ):
        broken = bytearray(valid)
        struct.pack_into("<" + fmt, broken, offset, value)
        try:
            validate(bytes(broken))
        except ValueError:
            continue
        raise AssertionError(f"mutation accepted at {offset}")

    fc2 = bytearray(fc2_validator.make_valid_report())
    struct.pack_into("<4Q", fc2, 160, 10, 55, 95, 130)
    bound = bytearray(valid)
    for phase_offset, fc2_offset in (
        (32, 24), (40, 32), (88, 56), (96, 48), (104, 64), (112, 128)
    ):
        struct.pack_into("<Q", bound, phase_offset, u64(fc2, fc2_offset))
    struct.pack_into("<Q", bound, 120, u64(fc2, 64) + 0xF64)
    struct.pack_into("<i", bound, 28, i32(fc2, 144))
    struct.pack_into("<II", bound, 372, u32(fc2, 148), u32(fc2, 148))
    cross_validate_fc2(validate(bytes(bound)), bytes(fc2))
    mismatched = bytearray(fc2)
    struct.pack_into("<Q", mismatched, 128, u64(mismatched, 128) + 8)
    try:
        cross_validate_fc2(validate(bytes(bound)), bytes(mismatched))
    except ValueError:
        pass
    else:
        raise AssertionError("cross-binding mutation accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("--fc2-report", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        print("FC3_PHASE_MAP_VALIDATOR_SELFTEST passed=1 size=408")
        return 0
    if args.report is None:
        parser.error("report is required unless --selftest is used")
    phase = validate(args.report.read_bytes())
    if args.fc2_report is not None:
        cross_validate_fc2(phase, args.fc2_report.read_bytes())
    print(f"FC3_PHASE_MAP_REPORT_PASS pid={phase['pid']} owner={phase['owner_tid']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
