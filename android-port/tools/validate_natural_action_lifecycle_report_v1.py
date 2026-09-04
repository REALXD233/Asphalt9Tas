#!/usr/bin/env python3
"""Strict validator for the zero-call lifecycle runtime report."""

from __future__ import annotations

import argparse
import ctypes
import pathlib
import struct
import tempfile


MAGIC = b"A9NALR1\0"
VERSION = 1
REQUIRED_FLAGS = 0x1FFFF
PHASE_CLEAN = 12
CLEANUP_NATURAL_REMOVAL_PROVED = 1
PAYLOAD_SHA256 = bytes.fromhex(
    "60a726174ac2a613d6098db77f9e82bbf819923ffd4e798d1f66eda9db454a41"
)
PAYLOAD_BUILD_ID = bytes.fromhex("39fbc246ac28635a7746088faf556cc85f5a2605")


class Evidence(ctypes.LittleEndianStructure):
    _fields_ = [
        ("magic", ctypes.c_char * 8),
        ("version", ctypes.c_uint32), ("size", ctypes.c_uint32),
        ("bootstrap_entries", ctypes.c_uint64),
        ("original_calls", ctypes.c_uint64),
        ("original_returns", ctypes.c_uint64),
        ("registration_attempts", ctypes.c_uint64),
        ("registration_returns", ctypes.c_uint64),
        ("callback_entries", ctypes.c_uint64),
        ("idle_entries", ctypes.c_uint64),
        ("claimed_commands", ctypes.c_uint64),
        ("zero_call_completions", ctypes.c_uint64),
        ("rejected_nonzero_commands", ctypes.c_uint64),
        ("removal_attempts", ctypes.c_uint64),
        ("removal_returns", ctypes.c_uint64),
        ("failures", ctypes.c_uint64),
        ("last_object", ctypes.c_uint64),
        ("last_token", ctypes.c_uint64),
        ("last_sequence", ctypes.c_uint64),
        ("last_frame", ctypes.c_uint32),
        ("last_tid", ctypes.c_uint32),
        ("last_status", ctypes.c_int32),
        ("protocol_state", ctypes.c_uint32),
        ("list_end_before_add", ctypes.c_uint64),
        ("list_active_before_add", ctypes.c_uint64),
        ("list_end_after_add", ctypes.c_uint64),
        ("list_active_after_add", ctypes.c_uint64),
        ("list_end_after_remove", ctypes.c_uint64),
        ("list_active_after_remove", ctypes.c_uint64),
        ("dispatch_before_add", ctypes.c_uint8),
        ("deferred_after_add", ctypes.c_uint8),
        ("dispatch_before_remove", ctypes.c_uint8),
        ("deferred_after_remove", ctypes.c_uint8),
        ("reserved_bytes", ctypes.c_uint8 * 4),
        ("reserved", ctypes.c_uint64 * 5),
    ]


class Report(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_char * 8),
        ("version", ctypes.c_uint32), ("size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32), ("final_phase", ctypes.c_uint32),
        ("pid", ctypes.c_uint64), ("process_generation", ctypes.c_uint64),
        ("game_library_base", ctypes.c_uint64),
        ("physics_context", ctypes.c_uint64),
        ("callback_list", ctypes.c_uint64),
        ("callback_flags", ctypes.c_uint64),
        ("car_physics_state", ctypes.c_uint64),
        ("original_vptr", ctypes.c_uint64),
        ("shadow_vptr", ctypes.c_uint64),
        ("payload_load_bias", ctypes.c_uint64),
        ("payload_shadow", ctypes.c_uint64),
        ("payload_control", ctypes.c_uint64),
        ("payload_evidence", ctypes.c_uint64),
        ("payload_mailbox", ctypes.c_uint64),
        ("dedicated_object", ctypes.c_uint64),
        ("dedicated_vptr", ctypes.c_uint64),
        ("bootstrap", ctypes.c_uint64),
        ("persistent_consumer", ctypes.c_uint64),
        ("fail_safe_callback", ctypes.c_uint64),
        ("session_id", ctypes.c_uint32), ("owner_tid", ctypes.c_int32),
        ("initial_threads", ctypes.c_uint32),
        ("final_threads", ctypes.c_uint32),
        ("cleanup_disposition", ctypes.c_uint32),
        ("reject_reasons", ctypes.c_uint32),
        ("ready_no_attach_ns", ctypes.c_uint64),
        ("resume_observed_ns", ctypes.c_uint64),
        ("registration_proved_ns", ctypes.c_uint64),
        ("mailbox_armed_ns", ctypes.c_uint64),
        ("zero_receipt_ns", ctypes.c_uint64),
        ("removal_requested_ns", ctypes.c_uint64),
        ("final_proof_ns", ctypes.c_uint64),
        ("game_write_attempts", ctypes.c_uint64),
        ("game_write_failures", ctypes.c_uint64),
        ("payload_write_attempts", ctypes.c_uint64),
        ("payload_write_failures", ctypes.c_uint64),
        ("ptrace_errors", ctypes.c_uint64),
        ("read_errors", ctypes.c_uint64),
        ("semantic_errors", ctypes.c_uint64),
        ("zero_call_frames", ctypes.c_uint64),
        ("rollback_attempts", ctypes.c_uint64),
        ("rollback_failures", ctypes.c_uint64),
        ("mailbox_claimed_sequence", ctypes.c_uint64),
        ("mailbox_completed_sequence", ctypes.c_uint64),
        ("evidence", Evidence),
        ("payload_sha256", ctypes.c_uint8 * 32),
        ("payload_build_id", ctypes.c_uint8 * 20),
        ("reserved", ctypes.c_uint32),
    ]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def raw_field(instance: ctypes.Structure, field: str, size: int) -> bytes:
    offset = getattr(type(instance), field).offset
    return ctypes.string_at(ctypes.addressof(instance) + offset, size)


def validate(report: Report) -> None:
    require(ctypes.sizeof(Evidence) == 256 and ctypes.sizeof(Report) == 664,
            "validator ABI mismatch")
    require(raw_field(report, "magic", 8) == MAGIC and
            report.version == VERSION and
            report.size == ctypes.sizeof(Report), "report header")
    require(report.flags == REQUIRED_FLAGS and report.final_phase == PHASE_CLEAN,
            "incomplete flags/final phase")
    for name in (
        "pid", "process_generation", "game_library_base", "physics_context",
        "callback_list", "callback_flags", "car_physics_state",
        "original_vptr", "shadow_vptr", "payload_load_bias",
        "payload_shadow", "payload_control", "payload_evidence",
        "payload_mailbox", "dedicated_object", "dedicated_vptr", "bootstrap",
        "persistent_consumer", "fail_safe_callback", "session_id", "owner_tid",
    ):
        require(getattr(report, name) != 0, f"zero identity field: {name}")
    require(report.initial_threads > 0 and
            report.final_threads == report.initial_threads,
            "thread ledger mismatch")
    require(report.cleanup_disposition == CLEANUP_NATURAL_REMOVAL_PROVED and
            report.reject_reasons == 0, "cleanup/reject disposition")
    times = [report.ready_no_attach_ns, report.resume_observed_ns,
             report.registration_proved_ns, report.mailbox_armed_ns,
             report.zero_receipt_ns, report.removal_requested_ns,
             report.final_proof_ns]
    require(all(times) and times == sorted(times), "timestamp ordering")
    require(report.game_write_attempts == 1 and
            report.game_write_failures == 0 and
            report.payload_write_attempts > 0 and
            report.payload_write_failures == 0 and
            report.ptrace_errors == report.read_errors ==
            report.semantic_errors == 0 and report.zero_call_frames > 0 and
            report.rollback_attempts == report.rollback_failures == 0,
            "counter invariant")
    require(report.mailbox_claimed_sequence == report.zero_call_frames and
            report.mailbox_completed_sequence == report.zero_call_frames,
            "mailbox final sequence")
    evidence = report.evidence
    require(raw_field(evidence, "magic", 8) == b"A9NAX1\0\0" and
            evidence.version == 1 and
            evidence.size == 256, "evidence header")
    require(evidence.bootstrap_entries == evidence.original_calls ==
            evidence.original_returns == evidence.registration_attempts ==
            evidence.registration_returns == 1, "registration counters")
    require(evidence.claimed_commands == evidence.zero_call_completions ==
            report.zero_call_frames and evidence.rejected_nonzero_commands == 0,
            "zero-call evidence")
    require(evidence.removal_attempts == evidence.removal_returns == 1 and
            evidence.failures == 0 and evidence.protocol_state == 3 and
            evidence.last_status == 3, "removal evidence")
    require(evidence.list_end_after_add == evidence.list_end_before_add + 16 and
            evidence.list_active_after_add == evidence.list_active_before_add and
            evidence.dispatch_before_add == evidence.deferred_after_add ==
            evidence.dispatch_before_remove == evidence.deferred_after_remove == 1,
            "deferred list evidence")
    require(bytes(report.payload_sha256) == PAYLOAD_SHA256 and
            bytes(report.payload_build_id) == PAYLOAD_BUILD_ID,
            "payload identity")
    require(report.reserved == 0 and not any(evidence.reserved_bytes) and
            not any(evidence.reserved), "reserved fields")


def valid_report() -> Report:
    report = Report()
    report.magic = MAGIC
    report.version = VERSION
    report.size = ctypes.sizeof(Report)
    report.flags = REQUIRED_FLAGS
    report.final_phase = PHASE_CLEAN
    for index, name in enumerate((
        "pid", "process_generation", "game_library_base", "physics_context",
        "callback_list", "callback_flags", "car_physics_state",
        "original_vptr", "shadow_vptr", "payload_load_bias",
        "payload_shadow", "payload_control", "payload_evidence",
        "payload_mailbox", "dedicated_object", "dedicated_vptr", "bootstrap",
        "persistent_consumer", "fail_safe_callback",
    ), 1):
        setattr(report, name, 0x1000 * index)
    report.session_id = 7
    report.owner_tid = 123
    report.initial_threads = report.final_threads = 9
    report.cleanup_disposition = CLEANUP_NATURAL_REMOVAL_PROVED
    for index, name in enumerate((
        "ready_no_attach_ns", "resume_observed_ns", "registration_proved_ns",
        "mailbox_armed_ns", "zero_receipt_ns", "removal_requested_ns",
        "final_proof_ns",
    ), 1):
        setattr(report, name, index * 10)
    report.game_write_attempts = 1
    report.payload_write_attempts = 5
    report.zero_call_frames = 1
    report.mailbox_claimed_sequence = report.mailbox_completed_sequence = 1
    evidence = report.evidence
    evidence.magic = b"A9NAX1\0\0"
    evidence.version = 1
    evidence.size = 256
    evidence.bootstrap_entries = evidence.original_calls = 1
    evidence.original_returns = evidence.registration_attempts = 1
    evidence.registration_returns = 1
    evidence.claimed_commands = evidence.zero_call_completions = 1
    evidence.removal_attempts = evidence.removal_returns = 1
    evidence.protocol_state = 3
    evidence.last_status = 3
    evidence.list_end_before_add = evidence.list_active_before_add = 0x2000
    evidence.list_end_after_add = 0x2010
    evidence.list_active_after_add = 0x2000
    evidence.dispatch_before_add = evidence.deferred_after_add = 1
    evidence.dispatch_before_remove = evidence.deferred_after_remove = 1
    report.payload_sha256[:] = PAYLOAD_SHA256
    report.payload_build_id[:] = PAYLOAD_BUILD_ID
    return report


def selftest() -> None:
    report = valid_report()
    validate(report)
    for field in ("flags", "game_write_attempts", "zero_call_frames",
                  "cleanup_disposition"):
        broken = Report.from_buffer_copy(bytes(report))
        setattr(broken, field, 0)
        try:
            validate(broken)
        except ValueError:
            continue
        raise AssertionError(f"validator accepted broken field: {field}")
    broken = Report.from_buffer_copy(bytes(report))
    broken.evidence.rejected_nonzero_commands = 1
    try:
        validate(broken)
    except ValueError:
        pass
    else:
        raise AssertionError("validator accepted rejected nonzero action")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        print("NATURAL_ACTION_LIFECYCLE_REPORT_SELFTEST passed=1 abi=664 "
              "zero_call_only=1 exact_cleanup=1")
        return 0
    require(args.report is not None, "report path required")
    data = args.report.read_bytes()
    require(len(data) == ctypes.sizeof(Report), "report file size")
    validate(Report.from_buffer_copy(data))
    print("NATURAL_ACTION_LIFECYCLE_REPORT_VALID passed=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
