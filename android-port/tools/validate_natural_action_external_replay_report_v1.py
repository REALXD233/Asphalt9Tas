#!/usr/bin/env python3
"""Validate dynamic A9NAR6 lifecycle receipts against one A9UTK1 source."""

from __future__ import annotations

import argparse
import ctypes
import pathlib

from unified_tick_recording_v1 import SKIP_NITRO, decode_recording
from validate_natural_action_lifecycle_report_v1 import Evidence, Report, raw_field


MAGIC = b"A9NAR6\0\0"
PAYLOAD_SHA256 = bytes.fromhex(
    "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
)
PAYLOAD_BUILD_ID = bytes.fromhex("6195c61b73505c9404dc11caff3bc6bb0a7e169a")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate(report: Report, recording: bytes) -> None:
    _, frames = decode_recording(recording)
    frame_count = len(frames)
    counts = [0 if frame.skip_flags & SKIP_NITRO else frame.nitro_activations
              for frame in frames]
    action_frames = sum(count != 0 for count in counts)
    action_calls = sum(counts)
    zero_frames = frame_count - action_frames
    require(ctypes.sizeof(Evidence) == 256 and ctypes.sizeof(Report) == 664,
            "validator ABI")
    require(raw_field(report, "magic", 8) == MAGIC and report.version == 1 and
            report.size == 664, "report header")
    require(report.flags == 0x1FFFF and report.final_phase == 12 and
            report.cleanup_disposition == 1 and report.reject_reasons == 0,
            "lifecycle completion")
    require(report.initial_threads > 0 and
            report.final_threads == report.initial_threads, "thread ledger")
    require(report.game_write_attempts == 1 and
            report.game_write_failures == report.payload_write_failures == 0 and
            report.payload_write_attempts > 0 and
            report.ptrace_errors == report.read_errors ==
            report.semantic_errors == 0 and
            report.rollback_attempts == report.rollback_failures == 0,
            "transport counters")
    # In the shared ABI these are named zero_call_frames and zero_receipt_ns;
    # A9NAR6 uses the same slots as action_frames/action_receipt_ns.
    require(report.zero_call_frames == frame_count and
            report.mailbox_claimed_sequence == frame_count and
            report.mailbox_completed_sequence == frame_count,
            "external replay cursor")
    evidence = report.evidence
    require(raw_field(evidence, "magic", 8) == b"A9NAX1\0\0" and
            evidence.version == 1 and evidence.size == 256, "evidence header")
    require(evidence.bootstrap_entries == evidence.original_calls ==
            evidence.original_returns == evidence.registration_attempts ==
            evidence.registration_returns == 1, "registration receipts")
    require(evidence.claimed_commands == frame_count and
            evidence.zero_call_completions == zero_frames and
            evidence.rejected_nonzero_commands == 0 and
            evidence.last_sequence == frame_count and
            evidence.last_frame + 1 == frame_count,
            "per-frame receipts")
    completions, calls, queue_before, queue_after, marker = (
        int(value) for value in evidence.reserved
    )
    require(completions == action_frames and calls == action_calls,
            "action totals")
    require(action_calls == 0 or
            (queue_before != 0 and queue_after != 0 and
             marker & 0xFFFF000000000000 == 0xA9E2000000000000),
            "game-owned action queue proof")
    require(evidence.removal_attempts == evidence.removal_returns == 1 and
            evidence.failures == 0 and evidence.protocol_state == 3 and
            evidence.last_status == 3 and
            evidence.dispatch_before_add == evidence.deferred_after_add ==
            evidence.dispatch_before_remove == evidence.deferred_after_remove == 1,
            "natural registration/removal")
    require(bytes(report.payload_sha256) == PAYLOAD_SHA256 and
            bytes(report.payload_build_id) == PAYLOAD_BUILD_ID,
            "payload identity")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("recording", type=pathlib.Path)
    args = parser.parse_args()
    try:
        data = args.report.read_bytes()
        require(len(data) == ctypes.sizeof(Report), "report size")
        validate(Report.from_buffer_copy(data), args.recording.read_bytes())
    except (OSError, ValueError) as error:
        print(f"a9nar6_error={error}")
        return 1
    print("NATURAL_ACTION_EXTERNAL_REPLAY_REPORT_VALID passed=1 "
          "per_frame_counts=1 natural_cleanup=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
