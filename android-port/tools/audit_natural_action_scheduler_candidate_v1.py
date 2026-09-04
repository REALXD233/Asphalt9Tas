#!/usr/bin/env python3
"""Cross-artifact audit for the local-only one-activation live candidate."""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys


EXPECTED_PAYLOAD_SHA256 = (
    "d718a13578e7e37a3d2a0240f94b125620858d9657d69a4fc799b8c5263b7f9e"
)
EXPECTED_PAYLOAD_BUILD_ID = "b172ed22adeba693673a5bf655c39db79f0b0f87"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def output(*args: str) -> str:
    return subprocess.check_output(args, text=True, errors="replace")


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: audit... CANDIDATE BOOTSTRAP PAYLOAD READELF SOURCE "
            "BOOTSTRAP_SOURCE BUILD_SCRIPT"
        )
    candidate, bootstrap, payload, readelf, source, bootstrap_source, build = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (candidate, bootstrap, payload, readelf, source,
                 bootstrap_source, build):
        require(path.is_file(), f"missing artifact: {path}")
    for artifact in (candidate, bootstrap):
        header = output(str(readelf), "-h", str(artifact))
        require("Advanced Micro Devices X86-64" in header and
                "Type:" in header and "DYN" in header,
                f"x86-64 PIE/DSO identity mismatch: {artifact}")
    require(hashlib.sha256(payload.read_bytes()).hexdigest() ==
            EXPECTED_PAYLOAD_SHA256, "action payload hash mismatch")
    require(EXPECTED_PAYLOAD_BUILD_ID in
            output(str(readelf), "-n", str(payload)).lower(),
            "action payload build ID mismatch")

    source_text = source.read_text(encoding="utf-8")
    bootstrap_text = bootstrap_source.read_text(encoding="utf-8")
    build_text = build.read_text(encoding="utf-8").lower()
    candidate_bytes = candidate.read_bytes()
    bootstrap_bytes = bootstrap.read_bytes()
    for token in (
        "A9TAS_NAL_ACTION_CONTROLLER_REVIEW",
        "action_owner != car",
        "I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1",
        "command.nitro_activations = 1",
        "kNitroOverrideEnabled",
        "action_command_completions",
        "action_calls_submitted",
        "kActionReceipt",
        "WaitForPositiveDelta",
        "RequestCleanRemoval",
        "PollForNaturalRemoval",
    ):
        require(token in source_text, f"action candidate source token missing: {token}")
    require("liba9tas_natural_action_scheduler_v1_review_only.so" in
            bootstrap_text, "bootstrap action payload path missing")
    require("natural_action_callback_lifecycle_v1_build_only.so" not in
            bootstrap_text, "passive payload leaked into action bootstrap source")
    for token in (
        b"I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1",
        b"NAL_READY_NO_ATTACH",
        b"paused_zero_samples=8",
    ):
        require(token in candidate_bytes, f"candidate token missing: {token!r}")
    require(b"liba9tas_natural_action_scheduler_v1_review_only.so" in
            bootstrap_bytes, "action bootstrap payload path missing from DSO")
    require(b"liba9tas_natural_action_callback_lifecycle_v1_build_only.so" not in
            bootstrap_bytes, "passive payload path leaked into action bootstrap")
    require("-da9tas_nal_action_controller_review=1" in build_text,
            "linked one-action controller macro missing")
    require("device_access=0" in build_text and "deployed=0" in build_text,
            "local-only build disposition missing")
    for forbidden in ("adb", "push", "install", "shell input", "keyevent"):
        require(forbidden not in build_text,
                f"local build script contains device operation: {forbidden}")
    print("NATURAL_ACTION_SCHEDULER_CANDIDATE_AUDIT passed=1 linked=1 "
          "activation_count=1 payload_identity=1 natural_cleanup=1 "
          "deployed=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
