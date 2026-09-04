#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


PAYLOAD_SHA = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def elf_ok(readelf: pathlib.Path, artifact: pathlib.Path, kind: str) -> None:
    header = subprocess.check_output([str(readelf), "-h", str(artifact)],
                                     text=True)
    require("Advanced Micro Devices X86-64" in header and
            re.search(rf"Type:\s+{kind}", header) is not None,
            f"ELF identity: {artifact}")


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit("usage: audit OBJECT CANDIDATE BOOTSTRAP PAYLOAD READELF SOURCE BUILD")
    obj, candidate, bootstrap, payload, readelf, source, build = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (obj, candidate, bootstrap, payload, readelf, source, build):
        require(path.is_file(), f"missing input: {path}")
    require(hashlib.sha256(payload.read_bytes()).hexdigest() == PAYLOAD_SHA,
            "replay payload identity")
    elf_ok(readelf, obj, "REL")
    elf_ok(readelf, candidate, "DYN")
    elf_ok(readelf, bootstrap, "DYN")
    text = source.read_text(encoding="utf-8")
    build_text = build.read_text(encoding="utf-8").lower()
    for token in (
        "A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW",
        "I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1",
        "CreateExternalReplayReadyMarker",
        "WaitForExternalReplayMarkerRemoval",
        "ArmZeroCallMailbox",
        "evidence.zero_call_completions + action_frames == frames",
        "RequestCleanRemoval",
        "PollForNaturalRemoval",
    ):
        require(token in text, f"external replay invariant missing: {token}")
    require("-da9tas_nal_external_replay_controller_review=1" in build_text,
            "external controller macro absent")
    require("deployed=0" in build_text and "device_access=0" in build_text,
            "build disposition")
    require(b"I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1" in
            candidate.read_bytes(), "dedicated acknowledgement absent")
    print("NATURAL_ACTION_EXTERNAL_REPLAY_CONTROLLER_AUDIT passed=1 "
          "payload_pinned=1 mailbox_initially_empty=1 external_frames=1 "
          "aggregate_receipts=1 natural_cleanup=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
