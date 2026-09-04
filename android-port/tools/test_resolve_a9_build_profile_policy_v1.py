#!/usr/bin/env python3
"""Policy guard for the channel-agnostic A9 native-build resolver."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    source = (ROOT / "tools/resolve_a9_build_profile_v1.py").read_text(
        encoding="utf-8"
    )
    required = (
        "A9_BUILD_PROFILE_RESOLUTION_V1",
        "ROLE_GROUPS",
        "choose_group_delta",
        "find_in_executable",
        "find_masked_in_executable",
        "ROLE_WORD_MASKS",
        "MAX_ROLE_LOCAL_DRIFT",
        "select_role_hit",
        "find_pointer_pair",
        "resolve_vtables",
        "pointer_occurrence_index",
        "map_reference_pointer",
        "direct_bl_callers",
        "branch_normalized_context_matches",
        "resolve_frame_event_scheduler_return",
        "resolve_lifecycle_vtables",
        "find_pointer_sequence",
        '"physics_interface_0"',
        '"physics_interface_8"',
        "multiple conflicting GNU build IDs",
        "os.replace(temporary, args.output)",
        "structural_vtable_score",
        '"channel_specific_literals": 0',
        '"write_authorized": False',
        "STATIC_STAGE2_PASS_LIVE_READONLY_REQUIRED",
        "live_object_graph_readback",
    )
    missing = [token for token in required if token not in source]
    if missing:
        raise SystemExit(f"build resolver policy missing: {missing}")
    forbidden = (
        "com.aligames.kuang.kybc.huawei",
        "com.huawei",
        "huawei-600300",
        "package_name ==",
        "channel_name ==",
        "process_vm_writev",
        "pwrite(",
        "ptrace(",
        "adb ",
    )
    present = [token for token in forbidden if token in source]
    if present:
        raise SystemExit(f"build resolver contains channel/write coupling: {present}")
    print(
        "A9_BUILD_PROFILE_RESOLVER_POLICY passed=1 channel_literals=0 "
        "offline_read_only=1 write_authorized=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
