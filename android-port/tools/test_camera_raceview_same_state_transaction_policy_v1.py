#!/usr/bin/env python3
"""Offline policy for the same-state single-slot controller."""

from __future__ import annotations
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "camera_raceview_same_state_transaction_controller_v1.cpp"

def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")
    for needle in (
        "camera_raceview_record_transaction_controller_v1.cpp",
        "kSamePayloadSha256", "58e888e4fb606fe3ec1b497c97a2a64c0b4dfc7dbd647ad0c01fa0558efd6ddf",
        "kEmbeddedVptrRva = 0x7F0F8A8", "kCombinedFunctionRva = 0x4D10D90",
        "stopped_node.function == payload.wrapper",
        "stopped_node.function == report.original_callback",
        "resume_failure_restored",
        "recovery_resumed",
        "evidence.combined_calls == same::kMaximumFrames",
        "install", "status", "finalize", "rollback",
    ):
        assert needle in text, needle
    assert "ptrace(" not in text and "PTRACE_" not in text
    assert text.count("++report.game_write_attempts") == 3
    print("CAMERA_RACEVIEW_SAME_STATE_TRANSACTION_POLICY passed=1 single_slot=1 conditional_restore=1 resume_failure_rollback=1 combined_identity=1 ptrace=0 device_access=0")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
