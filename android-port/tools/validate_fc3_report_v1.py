#!/usr/bin/env python3
"""Strict validator/selftest for A9FC3R1 and optional FC-2 cross-binding."""

from __future__ import annotations

import argparse
import math
import pathlib
import struct

import validate_fc2_report_v1 as fc2_validator


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc3_replay_observer_protocol_v1.h"
REPORT_SIZE = 512
REQUIRED_FLAGS = 0x3FFFF
PHASE_DETACHED = 6
SNAPSHOT_OFFSETS = (160, 224, 288, 352)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def verify_header_contract() -> None:
    text = HEADER.read_text(encoding="utf-8")
    for needle in (
        "constexpr std::uint32_t kRequiredFlags = 0x3FFFFu",
        "kPhaseProved = 5",
        "kDetached = 6",
        "phase_witness_accumulator",
        "static_assert(sizeof(Snapshot) == 64",
        "static_assert(sizeof(Report) == 512",
        "offsetof(Snapshot, callback_flags) == 12",
        "offsetof(Snapshot, direct_mode) == 14",
        "offsetof(Snapshot, nitro_active) == 15",
        "offsetof(Snapshot, accumulator_bits) == 20",
        "offsetof(Snapshot, action_queue_begin) == 24",
        "offsetof(Snapshot, dedicated_entries) == 56",
        "offsetof(Report, pid) == 24",
        "offsetof(Report, owner_tid) == 144",
        "offsetof(Report, dedicated) == 160",
        "offsetof(Report, callback_close) == 224",
        "offsetof(Report, next_accumulator) == 288",
        "offsetof(Report, next_callback_open) == 352",
        "offsetof(Report, dedicated_hits) == 416",
        "offsetof(Report, dedicated_members_final) == 488",
        "offsetof(Report, cleanup_disposition) == 492",
        "offsetof(Report, reserved) == 496",
    ):
        require(needle in text, f"FC-3 ABI token missing: {needle}")


def snapshot(data: bytes, offset: int) -> dict[str, int]:
    return {
        "time": u64(data, offset),
        "tid": i32(data, offset + 8),
        "flags": u16(data, offset + 12),
        "direct": data[offset + 14],
        "nitro_active": data[offset + 15],
        "nitro_mode": u32(data, offset + 16),
        "accumulator_bits": u32(data, offset + 20),
        "begin": u64(data, offset + 24),
        "end": u64(data, offset + 32),
        "capacity": u64(data, offset + 40),
        "count": u64(data, offset + 48),
        "dedicated_entries": u64(data, offset + 56),
    }


def validate(data: bytes) -> dict[str, int]:
    require(len(data) == REPORT_SIZE, "report size")
    require(data[0:8] == b"A9FC3R1\0", "report magic")
    require(u32(data, 8) == 1 and u32(data, 12) == REPORT_SIZE,
            "report version/declared size")
    flags = u32(data, 16)
    require(flags == REQUIRED_FLAGS, "required flags")
    require(u32(data, 20) == PHASE_DETACHED, "final phase")

    identities = [u64(data, 24 + 8 * index) for index in range(15)]
    (pid, base, context, callback_flags, callback_list, car,
     payload_evidence, dedicated_object, action_owner, action_queue_end,
     nitro_state, nitro_active_address, nitro_mode_address, world,
     phase_witness) = identities
    require(all(identities), "zero identity/address field")
    require(callback_list == context + 0x180 and
            callback_flags == context + 0x1A0,
            "callback identity relation")
    require(action_queue_end == action_owner + 0x1368,
            "action queue watch relation")
    require(nitro_active_address == nitro_state + 0x188 and
            nitro_mode_address == nitro_state + 0x18C,
            "Nitro watch-block relation")
    require(phase_witness == world + 0x188,
            "phase-witness relation")
    require(callback_flags % 2 == 0 and action_queue_end % 8 == 0 and
            nitro_active_address % 8 == 0 and phase_witness % 4 == 0,
            "watchpoint alignment")

    owner = i32(data, 144)
    initial_threads = u32(data, 148)
    final_threads = u32(data, 152)
    require(owner > 0 and initial_threads > 0 and
            final_threads == initial_threads, "thread accounting")
    require(u32(data, 156) == 0, "reject reasons")

    snapshots = [snapshot(data, offset) for offset in SNAPSHOT_OFFSETS]
    require([item["tid"] for item in snapshots] == [owner] * 4,
            "same owner across certified events")
    times = [item["time"] for item in snapshots]
    require(0 < times[0] < times[1] < times[2] < times[3],
            "event timestamp order")
    require([item["flags"] & 0xFF for item in snapshots] == [1, 0, 0, 1],
            "callback close/witness/open order")
    require(all(item["direct"] == 0 for item in snapshots),
            "direct mode changed")
    require(all(item["nitro_active"] == 0 for item in snapshots),
            "Nitro active changed")
    require(len({item["nitro_mode"] for item in snapshots}) == 1,
            "Nitro mode changed")
    queue_shapes: list[tuple[int, int, int, int]] = []
    for item in snapshots:
        require(item["begin"] <= item["end"] <= item["capacity"],
                "queue pointer order")
        require((item["end"] - item["begin"]) % 8 == 0 and
                (item["capacity"] - item["begin"]) % 8 == 0,
                "queue alignment")
        require(item["count"] ==
                (item["end"] - item["begin"]) // 8,
                "queue count")
        require(item["count"] == 0 and item["dedicated_entries"] == 1,
                "neutral-window precondition")
        queue_shapes.append((item["begin"], item["end"],
                             item["capacity"], item["count"]))
    require(len(set(queue_shapes)) == 1, "action queue mutated")
    accumulator = struct.unpack(
        "<f", struct.pack("<I", snapshots[2]["accumulator_bits"])
    )[0]
    require(math.isfinite(accumulator) and 0 < accumulator <= 1,
            "phase witness value")

    counters = [u64(data, 416 + 8 * index) for index in range(9)]
    require(counters == [1, 1, 0, 0, 1, 0, 0, 0, 0],
            "event/write/error counters")
    require(u32(data, 488) == 0 and u32(data, 492) == 0,
            "dedicated cleanup result")
    require(u64(data, 496) == 0 and u64(data, 504) == 0,
            "reserved trailer")
    return {
        "pid": pid,
        "base": base,
        "context": context,
        "callback_flags": callback_flags,
        "callback_list": callback_list,
        "car": car,
        "payload_evidence": payload_evidence,
        "dedicated_object": dedicated_object,
        "owner_tid": owner,
        "initial_threads": initial_threads,
        "flags": flags,
        "_dedicated_ns": times[0],
        "_callback_close_ns": times[1],
        "_accumulator_ns": times[2],
        "_next_callback_open_ns": times[3],
    }


def cross_validate_fc2(fc3: dict[str, int], fc2_data: bytes) -> None:
    # Cross-binding is meaningful only after the complete FC-2 success
    # contract (flags, phase, timestamps, counters, membership and payload
    # evidence) has independently passed.
    fc2_validator.validate(fc2_data)
    fc2 = {
        "pid": u64(fc2_data, 24),
        "base": u64(fc2_data, 32),
        "context": u64(fc2_data, 40),
        "callback_list": u64(fc2_data, 48),
        "callback_flags": u64(fc2_data, 56),
        "car": u64(fc2_data, 64),
        "payload_evidence": u64(fc2_data, 104),
        "dedicated_object": u64(fc2_data, 128),
        "owner_tid": i32(fc2_data, 144),
        "initial_threads": u32(fc2_data, 148),
    }
    require(fc2 == {key: fc3[key] for key in fc2},
            "FC-2/FC-3 identity or thread mismatch")
    require(u64(fc2_data, 192) == 1, "FC-2 one-write transaction missing")
    bootstrap_ns = u64(fc2_data, 160)
    registration_ns = u64(fc2_data, 168)
    removal_ns = u64(fc2_data, 176)
    compaction_ns = u64(fc2_data, 184)
    require(
        bootstrap_ns < registration_ns < fc3["_dedicated_ns"] <
        fc3["_callback_close_ns"] < removal_ns < fc3["_accumulator_ns"] <
        fc3["_next_callback_open_ns"] < compaction_ns,
        "FC-2/FC-3 transaction timeline mismatch",
    )


def make_valid_report() -> bytes:
    data = bytearray(REPORT_SIZE)
    data[0:8] = b"A9FC3R1\0"
    struct.pack_into("<IIII", data, 8, 1, REPORT_SIZE, REQUIRED_FLAGS,
                     PHASE_DETACHED)
    base = 0x700000000000
    context = 0x710000001000
    action_owner = 0x720000002000
    nitro_state = 0x730000003000
    world = 0x740000004000
    values = (
        1234, base, context, context + 0x1A0, context + 0x180,
        0x750000005000, 0x760000006000, 0x770000007000,
        action_owner, action_owner + 0x1368, nitro_state,
        nitro_state + 0x188, nitro_state + 0x18C, world, world + 0x188,
    )
    struct.pack_into("<15Q", data, 24, *values)
    struct.pack_into("<iIII", data, 144, 1300, 277, 277, 0)
    queue = (0x780000008000, 0x780000008000, 0x780000008040, 0, 1)
    for index, offset in enumerate(SNAPSHOT_OFFSETS):
        callback = (1, 0, 0, 1)[index]
        struct.pack_into("<QiHBBII5Q", data, offset, 100 + index, 1300,
                         callback, 0, 0, 0,
                         struct.unpack("<I", struct.pack("<f", 1 / 60))[0],
                         *queue)
    struct.pack_into("<9Q", data, 416, 1, 1, 0, 0, 1, 0, 0, 0, 0)
    return bytes(data)


def make_cross_bound_reports() -> tuple[bytes, bytes]:
    fc2 = bytearray(fc2_validator.make_valid_report())
    # FC-2 surrounds the four FC-3 certified stops in the same natural
    # registration/removal/compaction transaction.
    struct.pack_into("<4Q", fc2, 160, 10, 90, 250, 500)
    fc3 = bytearray(make_valid_report())
    mappings = (
        (24, 24),   # pid
        (32, 32),   # base
        (40, 40),   # PhysicsContext
        (48, 56),   # callback flags
        (56, 48),   # callback list
        (64, 64),   # car
        (72, 104),  # payload evidence
        (80, 128),  # dedicated object
    )
    for fc3_offset, fc2_offset in mappings:
        struct.pack_into("<Q", fc3, fc3_offset, u64(fc2, fc2_offset))
    owner = i32(fc2, 144)
    initial = u32(fc2, 148)
    struct.pack_into("<iII", fc3, 144, owner, initial, initial)
    for offset in SNAPSHOT_OFFSETS:
        struct.pack_into("<i", fc3, offset + 8, owner)
    for offset, timestamp in zip(SNAPSHOT_OFFSETS, (100, 200, 300, 400)):
        struct.pack_into("<Q", fc3, offset, timestamp)
    return bytes(fc3), bytes(fc2)


def selftest() -> None:
    verify_header_contract()
    valid = make_valid_report()
    validate(valid)
    mutations = {
        "flags": (16, "I", 0),
        "owner": (232, "i", 1301),
        "nitro": (SNAPSHOT_OFFSETS[2] + 15, "B", 1),
        "action_counter": (432, "Q", 1),
        "cleanup": (488, "I", 1),
    }
    for name, (offset, kind, value) in mutations.items():
        broken = bytearray(valid)
        struct.pack_into("<" + kind, broken, offset, value)
        try:
            validate(bytes(broken))
        except ValueError:
            continue
        raise AssertionError(f"selftest mutation accepted: {name}")
    bound_fc3, bound_fc2 = make_cross_bound_reports()
    cross_validate_fc2(validate(bound_fc3), bound_fc2)
    invalid_fc2 = bytearray(bound_fc2)
    struct.pack_into("<I", invalid_fc2, 16, 0)
    try:
        cross_validate_fc2(validate(bound_fc3), bytes(invalid_fc2))
    except ValueError:
        pass
    else:
        raise AssertionError("cross-validator accepted failed FC-2 report")
    stale_fc2 = bytearray(bound_fc2)
    struct.pack_into("<4Q", stale_fc2, 160, 1000, 1100, 1200, 1300)
    fc2_validator.validate(bytes(stale_fc2))
    try:
        cross_validate_fc2(validate(bound_fc3), bytes(stale_fc2))
    except ValueError:
        pass
    else:
        raise AssertionError("cross-validator accepted stale FC-2 transaction")

    def require_bound_rejection(fc3_bytes: bytes, fc2_bytes: bytes,
                                label: str) -> None:
        try:
            cross_validate_fc2(validate(fc3_bytes), fc2_bytes)
        except ValueError:
            return
        raise AssertionError(
            f"cross-validator accepted non-strict timeline edge: {label}"
        )

    # Attack every adjacent edge in the certified eight-point timeline.  Four
    # edges are internal to an individual strict report and three are visible
    # only when the two otherwise-valid reports are cross-bound.
    timeline_mutations = (
        ("bootstrap=registration", "fc2", 160, 90),
        ("registration=dedicated", "fc3", SNAPSHOT_OFFSETS[0], 90),
        ("dedicated=close", "fc3", SNAPSHOT_OFFSETS[1], 100),
        ("close=removal", "fc3", SNAPSHOT_OFFSETS[1], 250),
        ("removal=accumulator", "fc3", SNAPSHOT_OFFSETS[2], 250),
        ("accumulator=next-open", "fc3", SNAPSHOT_OFFSETS[3], 300),
        ("next-open=compaction", "fc3", SNAPSHOT_OFFSETS[3], 500),
    )
    for label, target, offset, timestamp in timeline_mutations:
        edge_fc3 = bytearray(bound_fc3)
        edge_fc2 = bytearray(bound_fc2)
        destination = edge_fc2 if target == "fc2" else edge_fc3
        struct.pack_into("<Q", destination, offset, timestamp)
        require_bound_rejection(bytes(edge_fc3), bytes(edge_fc2), label)
    print("FC3_REPORT_SELFTEST passed=1 size=512 strict_flags=1 "
          "sequence=dedicated-close-witness-open counters=zero-side-effect "
          "fc2_full_validation=1 timeline_adjacencies=7")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("--fc2-report", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--base", type=lambda value: int(value, 0))
    args = parser.parse_args()
    verify_header_contract()
    if args.selftest:
        selftest()
        return 0
    require(args.report is not None, "report path required")
    result = validate(args.report.read_bytes())
    if args.fc2_report is not None:
        cross_validate_fc2(result, args.fc2_report.read_bytes())
    if args.pid is not None:
        require(result["pid"] == args.pid, "requested pid identity")
    if args.base is not None:
        require(result["base"] == args.base, "requested base identity")
    print("FC3_REPORT_VALID passed=1 " + " ".join(
        f"{key}={hex(value) if key in ('flags', 'base') else value}"
        for key, value in result.items() if not key.startswith("_")
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
