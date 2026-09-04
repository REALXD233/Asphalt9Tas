#!/usr/bin/env python3
"""Static policy for the persistent Camera Tool command stream."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "camera_tool_stream_controller_v1.cpp"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")
    for needle in (
        "camera_tool_transaction_controller_v1.cpp",
        "I_ACCEPT_CAMERA_TOOL_PERSISTENT_STREAM_V1",
        "ReadConfigured",
        "PublishCommand",
        "WriteExactVerified",
        "offsetof(camera::Control, command_sequence)",
        "offsetof(camera::Control, override_flags)",
        "offsetof(camera::Control, position)",
        "offsetof(camera::Control, flags)",
        "evidence.failures != 0",
        "session.node.function == session.payload.wrapper",
        'PrintState("READY"',
        "CAMERA_TOOL_STREAM_SET",
        "CAMERA_TOOL_STREAM_TARGET",
        'std::strcmp(line, "TARGET\\n")',
        "vehicle_state_resolver_v1.h",
        "ResolveBackendLayout",
        "native_pose_address",
        "transform[12]",
        "transform[13]",
        "transform[14]",
        'PrintState("DISABLED"',
        "CAMERA_TOOL_STREAM_END",
        "payload_writes_only=1 game_writes=0 hook_installs=0",
    ):
        assert needle in text, needle
    for forbidden in (
        "StopProcess(", "ResumeProcess(", "callback::kCallbackOffset,\n+            &session.payload.wrapper", "process_vm_writev", "ptrace(",
        "input keyevent", "camera smoothing", "CameraReplay",
    ):
        assert forbidden not in text, forbidden
    assert text.index("&odd, sizeof(odd)") < text.index("&target, sizeof(target)")
    assert text.index("&target, sizeof(target)") < text.index("&even, sizeof(even)")
    assert text.index("&even, sizeof(even)") < text.index("&active, sizeof(active)")
    assert "if (!PublishInactive(session) && result == 0) result = 7;" in text
    print(
        "CAMERA_TOOL_STREAM_POLICY passed=1 persistent_su=1 "
        "seqlock=1 source_bound=1 payload_only=1 game_writes=0 hook_installs=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
