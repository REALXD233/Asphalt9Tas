#!/usr/bin/env python3
"""Guard the single-profile, channel-agnostic G8 integration path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    payload = (ROOT / "src/payload_g4_multi_hook_runtime_v1.cpp").read_text(
        encoding="utf-8"
    )
    controller = (ROOT / "src/g4_input_action_controller_v1.cpp").read_text(
        encoding="utf-8"
    )
    runner = (ROOT / "run-g4-input-action-gate-v1.ps1").read_text(
        encoding="utf-8"
    )
    lifecycle = (ROOT / "src/race_lifecycle_object_resolver_v1.h").read_text(
        encoding="utf-8"
    )
    required = {
        "payload": (
            "build_profile::Valid(g_build_profile)",
            "g_build_profile.hook_rvas[index]",
            "g_build_profile.physics_context_vtable_rva",
            "g_build_profile.frame_event_scheduler_return_rva",
            "a9tas_g4_build_profile_data_v1",
        ),
        "controller": (
            "ReadBuildProfileBundle",
            "kAnnexMagic",
            "g_build_profile.vehicle",
            "LifecycleProfile()",
            "PublishRemoteBuildProfile",
            "RemoteBuildProfileMatches",
            "g_build_profile.core.hook_rvas[index]",
        ),
        "runner": (
            "Select-ProfileForLiveImage",
            "profile_selection=native_sha",
            "No unique generated profile for native SHA",
            "a9tas_g8_runtime_build_profile_v1.bin",
        ),
        "lifecycle": (
            "struct Profile",
            "VtableAllowed",
            "lifecycle_vtable_count",
            "ResolveCountdownObject(pid_t pid, int mem, std::uintptr_t base,",
        ),
    }
    texts = {
        "payload": payload,
        "controller": controller,
        "runner": runner,
        "lifecycle": lifecycle,
    }
    missing = [
        f"{name}:{token}"
        for name, tokens in required.items()
        for token in tokens
        if token not in texts[name]
    ]
    if missing:
        raise SystemExit(f"G8 runtime profile integration missing: {missing}")
    forbidden_payload = (
        "kGameBuildId",
        "kTargetRvas",
        "game_base + kMainVtableRva",
        "game_base + kNitroServiceVtableRva",
    )
    forbidden_controller = (
        "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0",
        "e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b",
        "runtime.call.game_base + kHookRvas[index]",
        "runtime.call.game_base + protocol::kMainVtableRva",
    )
    present = [token for token in forbidden_payload if token in payload]
    present += [token for token in forbidden_controller if token in controller]
    if present:
        raise SystemExit(f"G8 retains reference-build runtime coupling: {present}")
    # Selection may expose package/process controls, but the build-profile
    # decision itself must be native-hash based and contain no channel branch.
    selection = runner[runner.index("function Select-ProfileForLiveImage"):]
    selection = selection[:selection.index("function Resolve-ExistingRemoteProfile")]
    for token in ("GamePackage", "huawei", "aligames", "package -eq"):
        if token in selection:
            raise SystemExit(f"build-profile selection is channel-coupled: {token}")
    print(
        "G8_RUNTIME_PROFILE_INTEGRATION_POLICY passed=1 single_runtime=1 "
        "selection=native_sha channel_branches=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
