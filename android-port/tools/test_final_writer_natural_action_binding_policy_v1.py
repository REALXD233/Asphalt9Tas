#!/usr/bin/env python3
"""Policy for the pure final-writer/natural-action composed cursor."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "final_writer_natural_action_binding_v1.h"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    for token in (
        "final_writer_cursor_binding_v1.h",
        "natural_action_replay_cursor_v1.h",
        "BeginFrame",
        "ObserveAction",
        "AcknowledgeWriter",
        "writer_evidence.processed_frames",
        "CanCommit",
        "action_.CanCommit(external_index)",
        "CommitFrame",
        "writer_.next_index() != action_.next_index()",
    ):
        require(token in text, f"composed binding invariant missing: {token}")
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
                f"live primitive or derived mode forbidden: {forbidden}")
    if len(sys.argv) == 2:
        require(pathlib.Path(sys.argv[1]).is_file(),
                "compiled composed-binding selftest object missing")
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [selftest]")
    print("FINAL_WRITER_NATURAL_ACTION_BINDING_POLICY passed=1 "
          "dual_receipt=1 exact_cursor=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
