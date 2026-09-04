#!/usr/bin/env python3
"""Reproducibly verify the exact Gate 5 Run 1 evidence pair."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from native_physics_recording_v1 import decode_recording, validate_runtime_safe
from parse_conditional_audit_v1 import assess, read_report


EXPECTED_RECORDING_SHA256 = (
    "933c4d1dffd087edab21efcd1c35f5e13079aa4f969e41f9ec42980a2eed7681"
)
EXPECTED_REPORT_SHA256 = (
    "431a20d399d2c210e4331fef9e51e50a99423cd85c5d999f4dddd5580f80b79a"
)
EXPECTED_FRAMES = 5


def verify_evidence(
    recording_path: Path,
    report_path: Path,
    *,
    expected_recording_hash: str | None = EXPECTED_RECORDING_SHA256,
    expected_report_hash: str | None = EXPECTED_REPORT_SHA256,
) -> list[str]:
    return verify_evidence_pair(
        recording_path,
        report_path,
        expected_recording_hash=expected_recording_hash,
        expected_report_hash=expected_report_hash,
        expected_equal=0,
        expected_corrected=5,
        expected_pid=2668,
        expected_initial_threads=270,
        expected_final_threads=270,
    )


def verify_evidence_pair(
    recording_path: Path,
    report_path: Path,
    *,
    expected_recording_hash: str | None,
    expected_report_hash: str | None,
    expected_equal: int,
    expected_corrected: int,
    expected_pid: int,
    expected_initial_threads: int,
    expected_final_threads: int,
) -> list[str]:
    problems: list[str] = []
    try:
        recording_blob = recording_path.read_bytes()
        report_blob = report_path.read_bytes()
    except OSError as error:
        return [f"evidence read failed: {error}"]

    recording_hash = hashlib.sha256(recording_blob).hexdigest()
    report_hash = hashlib.sha256(report_blob).hexdigest()
    if expected_recording_hash is not None and recording_hash != expected_recording_hash:
        problems.append(f"A9NPS1 hash mismatch: {recording_hash}")
    if expected_report_hash is not None and report_hash != expected_report_hash:
        problems.append(f"A9CDT1 hash mismatch: {report_hash}")

    try:
        recording = decode_recording(recording_blob)
        validate_runtime_safe(recording)
    except ValueError as error:
        problems.append(f"A9NPS1 rejected: {error}")
        return problems
    try:
        header, audits = read_report(report_path)
        problems.extend(
            assess(header, audits, minimum_corrected=1, maximum_frames=EXPECTED_FRAMES)
        )
    except ValueError as error:
        problems.append(f"A9CDT1 rejected: {error}")
        return problems

    if len(recording) != EXPECTED_FRAMES or len(audits) != EXPECTED_FRAMES:
        problems.append("Run 1 does not contain exactly five frames")
        return problems
    if (
        header.equal_frames != expected_equal
        or header.corrected_frames != expected_corrected
    ):
        problems.append(
            "evidence branch counts differ: "
            f"expected equal/corrected={expected_equal}/{expected_corrected}"
        )
    if header.write_attempts != expected_corrected:
        problems.append("write transactions do not equal corrected frames")
    if (
        header.pid != expected_pid
        or header.initial_threads != expected_initial_threads
        or header.final_threads != expected_final_threads
    ):
        problems.append("runtime identity/thread evidence differs")

    for index, (frame, audit) in enumerate(zip(recording, audits, strict=True)):
        if frame.tick != audit.tick:
            problems.append(f"frame {index}: recording/report tick mismatch")
        if frame.monotonic_ns != audit.monotonic_ns:
            problems.append(f"frame {index}: recording/report time mismatch")
        if frame.transform != audit.recorded_transform:
            problems.append(f"frame {index}: recording/report transform mismatch")
        if frame.linear_velocity != audit.recorded_linear:
            problems.append(f"frame {index}: recording/report linear mismatch")
    return problems


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "recording",
        nargs="?",
        type=Path,
        default=project / "evidence" / "a9tas_gate5_handoff_input_run1_20260817.a9nps1",
    )
    parser.add_argument(
        "report",
        nargs="?",
        type=Path,
        default=project / "evidence" / "a9tas_gate5_handoff_audit_run1_20260817.a9cdt1",
    )
    parser.add_argument("--require-supported", action="store_true")
    args = parser.parse_args()
    problems = verify_evidence(args.recording, args.report)
    supported = not problems
    print(
        f"gate5_run1_frames={EXPECTED_FRAMES} corrected=5 "
        f"recording_sha256={EXPECTED_RECORDING_SHA256} "
        f"report_sha256={EXPECTED_REPORT_SHA256} supported={int(supported)}"
    )
    for problem in problems:
        print(f"reject={problem}")
    return 0 if supported or not args.require_supported else 1


if __name__ == "__main__":
    raise SystemExit(main())
