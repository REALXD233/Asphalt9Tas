#!/usr/bin/env python3
"""Static policy for the Camera Tool single-slot host transaction."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "camera_tool_transaction_controller_v1.cpp"
PROTOCOL = ROOT / "src" / "camera_tool_protocol_v1.h"


def main() -> int:
    combined = SOURCE.read_text(encoding="utf-8") + PROTOCOL.read_text(encoding="utf-8")
    for needle in (
        "camera_raceview_record_transaction_controller_v1.cpp",
        "I_ACCEPT_CAMERA_TOOL_SINGLE_SLOT_V1",
        "liba9tas_camera_tool_v1_build_only.so",
        "7b2fad97e599943ff6c41d67611caeeb82d517fae5545c1e3f228371b3bdf5c8",
        "kWrapperRva = 0x1A50",
        "kControlPointerRva = 0x5000",
        "kEvidencePointerRva = 0x5140",
        "kControlStorageRva = 0x4F40",
        "kEvidenceStorageRva = 0x5040",
        "kSourceVptrRva = 0x9D5B768",
        "kPositionSetterRva = 0x4BF4D28",
        "kRotationSetterRva = 0x4BF4D7C",
        "kFovSetterRva = 0x4C711F8",
        "WriteExactVerified",
        "StopProcess(pid)",
        "ResumeProcess(pid)",
        "callback::kCallbackOffset",
        "offsetof(camera::Control, command_sequence)",
        "const std::uint64_t odd = control.command_sequence + 1",
        "const std::uint64_t even = control.command_sequence + 2",
        "Action::kActivate",
        "Action::kDeactivate",
        "Action::kUninstall",
    ):
        assert needle in combined, needle
    assert combined.count("node.node + callback::kCallbackOffset") >= 2
    assert "camera_replay" not in combined
    assert "A9G4R" not in combined
    assert "A9G5D" not in combined
    print(
        "CAMERA_TOOL_TRANSACTION_POLICY passed=1 existing_resolver_reused=1 "
        "single_slot=1 seqlock_publish=1 conditional_restore=1 replay_track=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
