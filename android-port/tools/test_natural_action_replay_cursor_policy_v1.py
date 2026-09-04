#!/usr/bin/env python3
"""Offline source policy for the natural-action/final-writer dual cursor."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "natural_action_replay_cursor_v1.h"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    for token in (
        "BuildFrameCommand",
        "frame.tick != external_index",
        "ObserveMailbox",
        "CompletionMatches",
        "expected_.nitro_activations",
        "AcknowledgeWriter",
        "writer_processed_frames != external_index + 1",
        "action_completed_ && writer_acknowledged_",
        "CanCommit",
        "CommitFrame",
    ):
        require(token in text, f"cursor invariant missing: {token}")
    for forbidden in (
        "NitroService",
        "kYellow",
        "kPerfectNitro",
        "kShockwave",
        "kRedNitro",
        "ptrace",
        "pwrite",
        "adb",
    ):
        require(forbidden.lower() not in text.lower(),
                f"live primitive or derived Nitro mode forbidden: {forbidden}")
    if len(sys.argv) == 2:
        require(pathlib.Path(sys.argv[1]).is_file(),
                "compiled cursor selftest object missing")
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [selftest]")
    print("NATURAL_ACTION_REPLAY_CURSOR_POLICY passed=1 exact_cursor=1 "
          "dual_receipt=1 colour_state_forced=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
