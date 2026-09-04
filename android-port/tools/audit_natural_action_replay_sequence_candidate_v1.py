#!/usr/bin/env python3
"""Cross-artifact audit for the local-only five-frame replay candidate."""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys


PAYLOAD_SHA = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
PAYLOAD_BUILD_ID = "6195c61b73505c9404dc11caff3bc6bb0a7e169a"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: audit CANDIDATE BOOTSTRAP PAYLOAD READELF SOURCE BOOTSTRAP_SOURCE BUILD"
        )
    candidate, bootstrap, payload, readelf, source, bootstrap_source, build = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (candidate, bootstrap, payload, readelf, source,
                 bootstrap_source, build):
        require(path.is_file(), f"missing artifact: {path}")
    for artifact in (candidate, bootstrap):
        header = subprocess.check_output([str(readelf), "-h", str(artifact)], text=True)
        require("Advanced Micro Devices X86-64" in header and "DYN" in header,
                f"x86-64 PIE/DSO identity: {artifact}")
    require(hashlib.sha256(payload.read_bytes()).hexdigest() == PAYLOAD_SHA,
            "payload hash")
    notes = subprocess.check_output([str(readelf), "-n", str(payload)], text=True).lower()
    require(PAYLOAD_BUILD_ID in notes, "payload build ID")
    source_text = source.read_text(encoding="utf-8")
    bootstrap_text = bootstrap_source.read_text(encoding="utf-8")
    build_text = build.read_text(encoding="utf-8").lower()
    for token in (
        "kReplaySequence[] = {0, 1, 0, 2, 0}",
        "I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1",
        "PublishZeroCall",
        "PollForZeroReceipt",
        "RequestCleanRemoval",
        "PollForNaturalRemoval",
    ):
        require(token in source_text, f"candidate invariant missing: {token}")
    require("liba9tas_natural_action_replay_v1_review_only.so" in bootstrap_text,
            "replay payload path")
    require("natural_action_scheduler_v1_review_only.so" not in bootstrap_text,
            "single Gate payload leaked")
    candidate_bytes = candidate.read_bytes()
    bootstrap_bytes = bootstrap.read_bytes()
    require(b"I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1" in candidate_bytes,
            "candidate acknowledgement")
    require(b"liba9tas_natural_action_replay_v1_review_only.so" in bootstrap_bytes,
            "bootstrap payload path")
    require("-da9tas_nal_sequence_controller_review=1" in build_text and
            "device_access=0" in build_text and "deployed=0" in build_text,
            "local-only build disposition")
    for forbidden in ("adb", " push ", "install", "keyevent"):
        require(forbidden not in build_text, f"device operation in build: {forbidden}")
    print("NATURAL_ACTION_REPLAY_SEQUENCE_CANDIDATE_AUDIT passed=1 linked=1 "
          "frames=5 sequence=0_1_0_2_0 payload_identity=1 natural_cleanup=1 "
          "deployed=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
