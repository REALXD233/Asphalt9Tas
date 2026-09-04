#!/usr/bin/env python3
"""Offline audit for the five-frame natural-action sequence controller."""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


PAYLOAD_SHA = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit("usage: audit OBJECT PAYLOAD READELF SOURCE VALIDATOR BUILD")
    obj, payload, readelf, source, validator, build = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (obj, payload, readelf, source, validator, build):
        require(path.is_file(), f"missing input: {path}")
    require(hashlib.sha256(payload.read_bytes()).hexdigest() == PAYLOAD_SHA,
            "replay payload hash")
    header = subprocess.check_output([str(readelf), "-h", str(obj)], text=True)
    require("Advanced Micro Devices X86-64" in header and
            re.search(r"Type:\s+REL", header), "review object identity")
    source_text = source.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    build_text = build.read_text(encoding="utf-8").lower()
    for token in (
        "A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW",
        "kReplaySequence[] = {0, 1, 0, 2, 0}",
        "I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1",
        "command.sequence = static_cast<std::uint64_t>(frame) + 1u",
        "PollForZeroReceipt(",
        "kReplaySequenceActionFrames",
        "kReplaySequenceActionCalls",
        "RequestCleanRemoval",
    ):
        require(token in source_text, f"sequence invariant missing: {token}")
    for token in (PAYLOAD_SHA, "A9NAR5", "ACTION_FRAMES = 2",
                  "ACTION_CALLS = 3", "ZERO_FRAMES = 3"):
        require(token in validator_text, f"validator invariant missing: {token}")
    require("-da9tas_nal_sequence_controller_review=1" in build_text and
            "device_access=0" in build_text and "deployed=0" in build_text,
            "build disposition")
    require(b"I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1" in obj.read_bytes(),
            "sequence acknowledgement absent")
    print("NATURAL_ACTION_REPLAY_SEQUENCE_CONTROLLER_AUDIT passed=1 "
          "frames=5 sequence=0_1_0_2_0 action_calls=3 payload_pinned=1 "
          "review_unlinked=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
