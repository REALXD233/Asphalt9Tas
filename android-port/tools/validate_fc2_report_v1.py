#!/usr/bin/env python3
"""Strict validator and selftest for the packed 528-byte A9FC2R1 report."""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc2_transaction_report_v1.h"
REPORT_SIZE = 528
EVIDENCE_OFFSET = 264
REQUIRED_FLAGS = 0xFFFF
PHASE_DETACHED = 11


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


@dataclasses.dataclass(frozen=True)
class Identity:
    pid: int
    library_base: int
    physics_context: int
    car: int
    original_vptr: int
    shadow_vptr: int
    dedicated_object: int
    dedicated_vptr: int


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def verify_header_contract() -> None:
    text = HEADER.read_text(encoding="utf-8")
    required = (
        "constexpr std::uint32_t kRequiredFlags = 0xFFFFu",
        "kWaitingBootstrapOpen = 1",
        "kWaitingRegistrationProof = 4",
        "kWaitingRemovalProof = 6",
        "kWaitingCompactionProof = 8",
        "kDetached = 11",
        "dedicated_members_before",
        "dedicated_members_after_registration",
        "dedicated_members_after_removal",
        "cleanup_disposition",
        "static_assert(sizeof(PayloadControl) == 128",
        "static_assert(sizeof(PayloadEvidence) == 256",
        "static_assert(sizeof(Report) == 528",
        "offsetof(PayloadEvidence, bootstrap_entries) == 16",
        "offsetof(PayloadEvidence, last_car) == 96",
        "offsetof(PayloadEvidence, last_original_result) == 168",
        "offsetof(PayloadEvidence, list_begin_before) == 176",
        "offsetof(PayloadEvidence, registration_dispatching_before) == 240",
        "offsetof(PayloadEvidence, last_status) == 248",
        "offsetof(Report, pid) == 24",
        "offsetof(Report, owner_tid) == 144",
        "offsetof(Report, bootstrap_open_ns) == 160",
        "offsetof(Report, game_write_attempts) == 192",
        "offsetof(Report, dedicated_members_before) == 248",
        "offsetof(Report, cleanup_disposition) == 260",
        "offsetof(Report, evidence) == 264",
        "offsetof(Report, reserved1) == 520",
    )
    for needle in required:
        require(needle in text, f"C++ report ABI token missing: {needle}")


def validate(data: bytes, expected: Identity | None = None) -> dict[str, int]:
    require(len(data) == REPORT_SIZE, "report size")
    require(data[0:8] == b"A9FC2R1\0", "report magic")
    require(u32(data, 8) == 1 and u32(data, 12) == REPORT_SIZE,
            "report version/declared size")
    flags = u32(data, 16)
    require(flags == REQUIRED_FLAGS, "required flags")
    require(u32(data, 20) == PHASE_DETACHED, "final phase")

    values = [u64(data, 24 + index * 8) for index in range(15)]
    (pid, base, context, callback_list, callback_flags, car, original_vptr,
     shadow_vptr, payload_shadow, payload_control, payload_evidence, wrapper,
     dedicated_observer, dedicated_object, dedicated_vptr) = values
    require(all(values), "zero identity/address field")
    require(callback_list == context + 0x180 and
            callback_flags == context + 0x1A0,
            "callback-list/flags relation")
    require(original_vptr == base + 0x7EE8D18,
            "original vptr relation")
    require(shadow_vptr == payload_shadow + 0x58,
            "shadow vptr relation")
    require(all(value & 7 == 0 for value in (
        context, callback_list, car, original_vptr, shadow_vptr,
        payload_shadow, payload_control, payload_evidence, dedicated_object,
        dedicated_vptr,
    )), "address alignment")
    require(wrapper & 3 == 0 and dedicated_observer & 3 == 0,
            "code alignment")

    owner_tid = i32(data, 144)
    initial_threads = u32(data, 148)
    final_threads = u32(data, 152)
    require(owner_tid > 0 and initial_threads > 0 and
            final_threads == initial_threads, "thread accounting")
    require(u32(data, 156) == 0, "reject reasons")

    times = [u64(data, 160 + index * 8) for index in range(4)]
    require(0 < times[0] < times[1] < times[2] < times[3],
            "three-frame timestamp order")
    counters = [u64(data, 192 + index * 8) for index in range(7)]
    require(counters == [1, 0, 0, 0, 0, 0, 0],
            "write/rollback/error counters")
    require([u32(data, 248 + index * 4) for index in range(4)] ==
            [0, 1, 0, 0], "membership/cleanup result")
    require(u64(data, 520) == 0, "reserved1")

    evidence = data[EVIDENCE_OFFSET:EVIDENCE_OFFSET + 256]
    require(evidence[0:8] == b"A9FC2E1\0", "evidence magic")
    require(u32(evidence, 8) == 1 and u32(evidence, 12) == 256,
            "evidence version/size")
    evidence_counters = [u64(evidence, 16 + index * 8) for index in range(10)]
    require(evidence_counters == [1, 1, 1, 1, 1, 1, 1, 1, 0, 0],
            "payload evidence counters")
    identity_fields = [u64(evidence, 96 + index * 8) for index in range(9)]
    (last_car, bootstrap_token, observed_vptr, restored_vptr,
     observed_context_vptr, observed_add, observed_remove,
     last_dedicated_object, dedicated_token) = identity_fields
    require(last_car == car and bootstrap_token != 0 and
            observed_vptr == shadow_vptr and restored_vptr == original_vptr,
            "bootstrap evidence identity")
    require(observed_context_vptr == base + 0x8103830 and
            observed_add == base + 0x38B77CC and
            observed_remove == base + 0x38B7840,
            "context mutation method identity")
    require(last_dedicated_object == dedicated_object and
            dedicated_token != 0, "dedicated evidence identity")

    list_values = [u64(evidence, 176 + index * 8) for index in range(8)]
    (begin_before, end_before, active_before,
     begin_after_add, end_after_add, active_after_add,
     begin_after_remove, end_after_remove) = list_values
    require(begin_before != 0 and begin_before <= active_before <= end_before,
            "pre-add list shape")
    require(begin_after_add != 0 and
            end_after_add - begin_after_add ==
            end_before - begin_before + 16 and
            active_after_add - begin_after_add ==
            active_before - begin_before,
            "deferred-add list delta")
    require(begin_after_remove != 0 and
            end_after_remove >= begin_after_remove,
            "post-remove list shape")
    require(list(evidence[240:248]) == [1, 0, 1, 1, 1, 0, 1, 1],
            "dispatch/deferred evidence")
    require(i32(evidence, 248) == 2 and u32(evidence, 252) == 2,
            "payload final status/protocol")

    if expected is not None:
        require((pid, base, context, car, original_vptr, shadow_vptr,
                 dedicated_object, dedicated_vptr) == dataclasses.astuple(expected),
                "expected target identity")
    return {
        "pid": pid,
        "base": base,
        "owner_tid": owner_tid,
        "flags": flags,
        "game_writes": counters[0],
    }


def make_valid_report() -> bytes:
    data = bytearray(REPORT_SIZE)
    data[0:8] = b"A9FC2R1\0"
    struct.pack_into("<IIII", data, 8, 1, REPORT_SIZE, REQUIRED_FLAGS,
                     PHASE_DETACHED)
    base = 0x700000000000
    context = 0x710000001000
    car = 0x720000002000
    payload_shadow = 0x730000003000
    values = (
        1234, base, context, context + 0x180, context + 0x1A0, car,
        base + 0x7EE8D18, payload_shadow + 0x58, payload_shadow,
        0x730000004000, 0x730000005000, 0x730000006000,
        0x730000007000, 0x730000008000, 0x730000009010,
    )
    struct.pack_into("<15Q", data, 24, *values)
    struct.pack_into("<iIII", data, 144, 1300, 277, 277, 0)
    struct.pack_into("<4Q", data, 160, 100, 200, 300, 400)
    struct.pack_into("<7Q", data, 192, 1, 0, 0, 0, 0, 0, 0)
    struct.pack_into("<4I", data, 248, 0, 1, 0, 0)

    evidence = memoryview(data)[EVIDENCE_OFFSET:EVIDENCE_OFFSET + 256]
    evidence[0:8] = b"A9FC2E1\0"
    struct.pack_into("<II", evidence, 8, 1, 256)
    struct.pack_into("<10Q", evidence, 16, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0)
    struct.pack_into("<9Q", evidence, 96,
                     car, 0x740000001000, payload_shadow + 0x58,
                     base + 0x7EE8D18, base + 0x8103830,
                     base + 0x38B77CC, base + 0x38B7840,
                     0x730000008000, 0x740000002000)
    struct.pack_into("<Q", evidence, 168, 0)
    struct.pack_into("<8Q", evidence, 176,
                     0x750000000000, 0x750000000070, 0x750000000070,
                     0x750000001000, 0x750000001080, 0x750000001070,
                     0x750000001000, 0x750000001080)
    evidence[240:248] = bytes([1, 0, 1, 1, 1, 0, 1, 1])
    struct.pack_into("<iI", evidence, 248, 2, 2)
    return bytes(data)


def selftest() -> None:
    verify_header_contract()
    valid = make_valid_report()
    validate(valid)
    mutations = {
        "flags": (16, 0),
        "membership": (252, 2),
        "dedicated_count": (EVIDENCE_OFFSET + 56, 2),
        "deferred_after_remove": (EVIDENCE_OFFSET + 247, 0),
        "reserved": (520, 1),
    }
    for name, (offset, value) in mutations.items():
        broken = bytearray(valid)
        if offset in (EVIDENCE_OFFSET + 247,):
            broken[offset] = value
        elif offset in (16, 252):
            struct.pack_into("<I", broken, offset, value)
        else:
            struct.pack_into("<Q", broken, offset, value)
        try:
            validate(bytes(broken))
        except ValueError:
            continue
        raise AssertionError(f"selftest mutation accepted: {name}")
    with tempfile.TemporaryDirectory() as unused:
        require(bool(unused), "temporary selftest")
    print("FC2_REPORT_SELFTEST passed=1 size=528 strict_flags=1 "
          "three_frame_order=1 membership=1 evidence=1")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", type=pathlib.Path)
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
    if args.pid is not None:
        require(result["pid"] == args.pid, "requested pid identity")
    if args.base is not None:
        require(result["base"] == args.base, "requested base identity")
    print("FC2_REPORT_VALID passed=1 " + " ".join(
        f"{key}={hex(value) if key in ('flags', 'base') else value}"
        for key, value in result.items()
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
