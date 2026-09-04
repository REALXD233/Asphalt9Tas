#!/usr/bin/env python3
"""Bind A9USR3 action-window semantics to its neutral A9NPA1 anchor."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from natural_preroll_anchor_v1 import decode_anchor, verify_source_binding
from synchronized_action_window_recording_v1 import verify_action_window_capture
from synchronized_action_until_release_recording_v1 import (
    MAGIC as ACTION_RELEASE_MAGIC,
    verify_action_until_release_capture,
)


def verify(report_blob: bytes, recording_blob: bytes, anchor_blob: bytes) -> None:
    if report_blob[:8] == ACTION_RELEASE_MAGIC:
        report = verify_action_until_release_capture(
            report_blob, recording_blob
        ).report
    else:
        report = verify_action_window_capture(report_blob, recording_blob)
    anchor = decode_anchor(anchor_blob)
    if anchor.c98_pair_after != 0 or anchor.c9c_pair_after != 0:
        raise ValueError("action-window anchor control pair is not neutral")
    if anchor.events[-1] >= report.frames[0].events[0]:
        raise ValueError("action-window anchor does not precede frame 0")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    parser.add_argument("anchor", type=Path)
    args = parser.parse_args()
    try:
        verify_source_binding(
            decode_anchor(args.anchor.read_bytes()), args.report, args.recording
        )
        verify(
            args.report.read_bytes(),
            args.recording.read_bytes(),
            args.anchor.read_bytes(),
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"action_window_anchor_error={error}")
        return 1
    print("action_window_anchor_supported=1 neutral=1 ordered=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
