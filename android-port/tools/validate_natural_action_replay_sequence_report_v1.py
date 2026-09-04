#!/usr/bin/env python3
"""Strict validator for the 0/1/0/2/0 natural-action replay Gate."""

from __future__ import annotations

import argparse
import ctypes
import pathlib

from validate_natural_action_lifecycle_report_v1 import Evidence, Report, raw_field


MAGIC = b"A9NAR5\0\0"
REQUIRED_FLAGS = 0x1FFFF
PHASE_CLEAN = 12
CLEANUP_NATURAL_REMOVAL_PROVED = 1
PAYLOAD_SHA256 = bytes.fromhex(
    "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
)
PAYLOAD_BUILD_ID = bytes.fromhex("6195c61b73505c9404dc11caff3bc6bb0a7e169a")
FRAME_COUNT = 5
ACTION_FRAMES = 2
ACTION_CALLS = 3
ZERO_FRAMES = 3


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate(report: Report) -> None:
    require(ctypes.sizeof(Evidence) == 256 and ctypes.sizeof(Report) == 664,
            "validator ABI mismatch")
    require(raw_field(report, "magic", 8) == MAGIC and
            report.version == 1 and report.size == 664, "report header")
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
            report.final_threads == report.initial_threads, "thread ledger")
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
            report.semantic_errors == 0 and
            report.zero_call_frames == FRAME_COUNT and
            report.rollback_attempts == report.rollback_failures == 0,
            "report counters")
    require(report.mailbox_claimed_sequence == FRAME_COUNT and
            report.mailbox_completed_sequence == FRAME_COUNT,
            "mailbox final cursor")

    evidence = report.evidence
    require(raw_field(evidence, "magic", 8) == b"A9NAX1\0\0" and
            evidence.version == 1 and evidence.size == 256,
            "evidence header")
    require(evidence.bootstrap_entries == evidence.original_calls ==
            evidence.original_returns == evidence.registration_attempts ==
            evidence.registration_returns == 1, "registration counters")
    require(evidence.callback_entries >= FRAME_COUNT and
            evidence.claimed_commands == FRAME_COUNT and
            evidence.zero_call_completions == ZERO_FRAMES and
            evidence.rejected_nonzero_commands == 0,
            "five-frame command receipts")
    require(evidence.last_sequence == FRAME_COUNT and
            evidence.last_frame == FRAME_COUNT - 1 and evidence.last_tid != 0,
            "last command identity")
    require(evidence.removal_attempts == evidence.removal_returns == 1 and
            evidence.failures == 0 and evidence.protocol_state == 3 and
            evidence.last_status == 3, "removal evidence")
    action_completions, action_calls, queue_before, queue_after, marker = (
        int(value) for value in evidence.reserved
    )
    require(action_completions == ACTION_FRAMES and
            action_calls == ACTION_CALLS, "0/1/0/2/0 action totals")
    require(queue_before != 0 and queue_after != 0,
            "game-owned action queue evidence")
    require((marker & 0xFFFF000000000000) == 0xA9E2000000000000 and
            not any(evidence.reserved_bytes), "submission proof marker")
    require(evidence.list_end_after_add == evidence.list_end_before_add + 16 and
            evidence.list_active_after_add == evidence.list_active_before_add and
            evidence.dispatch_before_add == evidence.deferred_after_add ==
            evidence.dispatch_before_remove == evidence.deferred_after_remove == 1,
            "deferred list evidence")
    require(bytes(report.payload_sha256) == PAYLOAD_SHA256 and
            bytes(report.payload_build_id) == PAYLOAD_BUILD_ID,
            "payload identity")
    require(report.reserved == 0, "reserved report field")


def valid_report() -> Report:
    report = Report()
    report.magic = MAGIC
    report.version = 1
    report.size = 664
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
        setattr(report, name, index * 0x1000)
    report.session_id = 9
    report.owner_tid = 321
    report.initial_threads = report.final_threads = 8
    report.cleanup_disposition = CLEANUP_NATURAL_REMOVAL_PROVED
    for index, name in enumerate((
        "ready_no_attach_ns", "resume_observed_ns", "registration_proved_ns",
        "mailbox_armed_ns", "zero_receipt_ns", "removal_requested_ns",
        "final_proof_ns",
    ), 1):
        setattr(report, name, index * 10)
    report.game_write_attempts = 1
    report.payload_write_attempts = 3 + 3 * FRAME_COUNT
    report.zero_call_frames = FRAME_COUNT
    report.mailbox_claimed_sequence = report.mailbox_completed_sequence = FRAME_COUNT
    evidence = report.evidence
    evidence.magic = b"A9NAX1\0\0"
    evidence.version = 1
    evidence.size = 256
    evidence.bootstrap_entries = evidence.original_calls = 1
    evidence.original_returns = evidence.registration_attempts = 1
    evidence.registration_returns = 1
    evidence.callback_entries = FRAME_COUNT
    evidence.claimed_commands = FRAME_COUNT
    evidence.zero_call_completions = ZERO_FRAMES
    evidence.last_sequence = FRAME_COUNT
    evidence.last_frame = FRAME_COUNT - 1
    evidence.last_tid = report.owner_tid
    evidence.removal_attempts = evidence.removal_returns = 1
    evidence.protocol_state = 3
    evidence.last_status = 3
    evidence.list_end_before_add = evidence.list_active_before_add = 0x2000
    evidence.list_end_after_add = 0x2010
    evidence.list_active_after_add = 0x2000
    evidence.dispatch_before_add = evidence.deferred_after_add = 1
    evidence.dispatch_before_remove = evidence.deferred_after_remove = 1
    evidence.reserved[0] = ACTION_FRAMES
    evidence.reserved[1] = ACTION_CALLS
    evidence.reserved[2] = 0x3000
    evidence.reserved[3] = 0x3010
    evidence.reserved[4] = 0xA9E2000000000000
    report.payload_sha256[:] = PAYLOAD_SHA256
    report.payload_build_id[:] = PAYLOAD_BUILD_ID
    return report


def selftest() -> None:
    report = valid_report()
    validate(report)
    for mutate in (
        lambda item: setattr(item, "zero_call_frames", 4),
        lambda item: setattr(item, "mailbox_completed_sequence", 4),
        lambda item: setattr(item.evidence, "zero_call_completions", 2),
        lambda item: item.evidence.reserved.__setitem__(0, 1),
        lambda item: item.evidence.reserved.__setitem__(1, 2),
        lambda item: item.evidence.reserved.__setitem__(4, 0),
    ):
        broken = Report.from_buffer_copy(bytes(report))
        mutate(broken)
        try:
            validate(broken)
        except ValueError:
            continue
        raise AssertionError("validator accepted broken sequence evidence")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        print("NATURAL_ACTION_REPLAY_SEQUENCE_REPORT_SELFTEST passed=1 "
              "frames=5 sequence=0_1_0_2_0 action_calls=3 cleanup=1")
        return 0
    require(args.report is not None, "report path required")
    data = args.report.read_bytes()
    require(len(data) == ctypes.sizeof(Report), "report file size")
    validate(Report.from_buffer_copy(data))
    print("NATURAL_ACTION_REPLAY_SEQUENCE_REPORT_VALID passed=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
