#!/usr/bin/env python3
"""Cross-artifact audit for the local-only zero-call lifecycle candidate."""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys


EXPECTED_PAYLOAD_SHA256 = (
    "60a726174ac2a613d6098db77f9e82bbf819923ffd4e798d1f66eda9db454a41"
)
EXPECTED_PAYLOAD_BUILD_ID = "39fbc246ac28635a7746088faf556cc85f5a2605"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def output(*args: str) -> str:
    return subprocess.check_output(args, text=True, errors="replace")


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: audit... CANDIDATE PAYLOAD READELF SOURCE BUILD_SCRIPT"
        )
    candidate, payload, readelf, source, build_script = map(
        pathlib.Path, sys.argv[1:]
    )
    for path in (candidate, payload, readelf, source, build_script):
        require(path.is_file(), f"missing artifact: {path}")

    candidate_header = output(str(readelf), "-h", str(candidate))
    require("Advanced Micro Devices X86-64" in candidate_header,
            "candidate machine mismatch")
    require("Type:" in candidate_header and "DYN" in candidate_header,
            "candidate is not a PIE/DYN executable")

    payload_hash = hashlib.sha256(payload.read_bytes()).hexdigest()
    require(payload_hash == EXPECTED_PAYLOAD_SHA256,
            f"payload hash mismatch: {payload_hash}")
    payload_notes = output(str(readelf), "-n", str(payload)).lower()
    require(EXPECTED_PAYLOAD_BUILD_ID in payload_notes,
            "payload build ID mismatch")

    source_text = source.read_text(encoding="utf-8")
    build_text = build_script.read_text(encoding="utf-8").lower()
    candidate_bytes = candidate.read_bytes()
    for token in (
        "I_ACCEPT_ZERO_CALL_LIFECYCLE_REVIEW_V1",
        "NAL_READY_NO_ATTACH",
        "EstablishPausedDeltaBaseline",
        "paused_zero_observations == 8",
        "WaitForPositiveDelta",
        "CreateReadyMarker",
        "target_threads_attached=0 game_writes=0",
        "protocol->resume_observations == 1",
        "protocol->attach_permissions == 1",
        "ArmZeroCallMailbox",
        "PublishZeroCall",
        "RequestCleanRemoval",
        "PollForNaturalRemoval",
        "WriteLifecycleReportExclusive",
        "command.nitro_activations != 0",
    ):
        require(token in source_text, f"source lifecycle token missing: {token}")
    for token in (
        b"I_ACCEPT_ZERO_CALL_LIFECYCLE_REVIEW_V1",
        b"NAL_READY_NO_ATTACH",
        b"paused_zero_samples=8",
    ):
        require(token in candidate_bytes,
                f"linked candidate token missing: {token!r}")

    for forbidden in (
        b"SpoofCallToEnableNitroFunction",
        b"NitroService",
        b"dispatch_action",
        b"input keyevent",
    ):
        require(forbidden not in candidate_bytes,
                f"candidate contains forbidden action path: {forbidden!r}")
    for forbidden in ("adb", "push", "install", "shell input", "keyevent"):
        require(forbidden not in build_text,
                f"local build script contains device operation: {forbidden}")
    require("-da9tas_nal_controller_review=1" in build_text,
            "linked review macro missing")
    require("device_access=0" in build_text and "deployed=0" in build_text,
            "local-only disposition missing")

    print(
        "NATURAL_ACTION_LIFECYCLE_CANDIDATE_AUDIT passed=1 "
        "linked=1 local_only=1 payload_identity=1 zero_call_only=1 "
        "positive_delta_gate=1 action_calls=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
