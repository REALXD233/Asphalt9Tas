#!/usr/bin/env python3
"""Offline source and executable policy for per-frame natural Nitro replay."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "natural_action_replay_transport_v1.h"
UPSTREAM = ROOT.parent / "ref-alu-tas-v2" / "DetourFunctions.cpp"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    upstream = UPSTREAM.read_text(encoding="utf-8")
    for token in (
        "nitro_activation_count",
        "kMaximumNitroActivations",
        "sequence != frame.tick + 1",
        "kSkipNitroActivation",
        "kNitroOverrideEnabled",
        "ActionTransitionComplete",
        "before.active != after.active",
        "before.mode != after.mode",
    ):
        require(token in text, f"transport token missing: {token}")
    for forbidden in (
        "NitroService",
        "kYellow",
        "kPerfectNitro",
        "kShockwave",
        "kRedNitro",
        "forced_mode",
        "nitro_mode =",
        "ptrace",
        "pwrite",
        "adb",
    ):
        require(forbidden.lower() not in text.lower(),
                f"forbidden derived-mode or live primitive: {forbidden}")
    require("m_nitro_activation_count_this_frame++" in upstream and
            "SpoofCallToEnableNitroFunction" in upstream,
            "upstream activation-count evidence missing")
    if len(sys.argv) == 2:
        require(pathlib.Path(sys.argv[1]).is_file(),
                "compiled transport selftest object missing")
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [selftest]")
    print("NATURAL_ACTION_REPLAY_TRANSPORT_POLICY passed=1 "
          "upstream_counts=1 colour_state_forced=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
