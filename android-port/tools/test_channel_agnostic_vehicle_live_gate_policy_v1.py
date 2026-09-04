#!/usr/bin/env python3
"""Policy guard for the profile-driven, read-only vehicle live Gate."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    resolver = (ROOT / "tools/resolve_a9_build_profile_v1.py").read_text(
        encoding="utf-8"
    )
    header = (ROOT / "src/vehicle_state_resolver_v1.h").read_text(
        encoding="utf-8"
    )
    gate = (ROOT / "src/channel_agnostic_vehicle_live_gate_v1.cpp").read_text(
        encoding="utf-8"
    )
    runner = (ROOT / "run-channel-agnostic-vehicle-live-gate-v1.ps1").read_text(
        encoding="utf-8"
    )
    combined = resolver + header + gate + runner
    required = (
        '"physics_interface_0"',
        '"physics_interface_8"',
        "relation_delta_error",
        "struct Profile",
        "const Profile& profile",
        "O_RDONLY | O_CLOEXEC",
        "game_writes=0 ptrace_calls=0 hooks=0 auto_esc=0",
        "[string]$GamePackage",
        "[string]$ProfilePath",
        "[string]$GameProcessName",
        "[int]$TargetProcessId",
        "channel_specific_literals -ne 0",
        "Profile candidate ELF hash mismatch",
        "TracerPid",
        "ValidateMainObject",
        "ValidateImplementation",
        "ValidateLifecycle",
        "lifecycle_vtables",
        "main_candidates=",
    )
    missing = [token for token in required if token not in combined]
    if missing:
        raise SystemExit(f"channel-agnostic live-gate policy missing: {missing}")
    forbidden = (
        "com.aligames.kuang.kybc.huawei",
        "huawei-600300",
        "PTRACE_ATTACH",
        "process_vm_writev",
        "pwrite(",
        "input keyevent",
        "input tap",
        "am force-stop",
    )
    present = [token for token in forbidden if token in gate + runner]
    if present:
        raise SystemExit(
            f"channel-agnostic live Gate contains channel/write coupling: {present}"
        )
    print(
        "CHANNEL_AGNOSTIC_VEHICLE_LIVE_GATE_POLICY passed=1 "
        "runtime_profile=1 read_only=1 channel_literals=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
