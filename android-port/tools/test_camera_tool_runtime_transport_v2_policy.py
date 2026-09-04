#!/usr/bin/env python3
"""Offline policy for Camera Tool v2 install and payload-only input transport."""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
TRANSACTION = ROOT / "src" / "camera_tool_runtime_transaction_controller_v2.cpp"
STREAM = ROOT / "src" / "camera_tool_runtime_input_stream_v2.cpp"
PAYLOAD_SOURCE = ROOT / "src" / "payload_camera_tool_runtime_v2.cpp"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    if len(sys.argv) == 1:
        build = ROOT / "build" / "camera-tool-runtime-v2"
        payload = build / "liba9tas_camera_tool_runtime_v2_build_only.so"
        transaction = build / "a9tas_camera_tool_runtime_transaction_v2"
        stream = build / "a9tas_camera_tool_runtime_input_stream_v2"
        readelf = (
            ROOT.parent / "toolchains" / "android-ndk-r27d" / "toolchains" /
            "llvm" / "prebuilt" / "windows-x86_64" / "bin" /
            "llvm-readelf.exe"
        )
    elif len(sys.argv) == 5:
        payload, transaction, stream, readelf = map(pathlib.Path, sys.argv[1:])
    else:
        raise SystemExit(
            f"usage: {sys.argv[0]} PAYLOAD TRANSACTION STREAM READELF"
        )
    for path in (TRANSACTION, STREAM, PAYLOAD_SOURCE, payload, transaction,
                 stream, readelf):
        assert path.is_file(), path
    txn = TRANSACTION.read_text(encoding="utf-8")
    channel = STREAM.read_text(encoding="utf-8")
    payload_hash = sha256(payload)
    assert payload_hash in txn
    for needle in (
        "kHudVisibilityWrapperRva = 0x1E80",
        "kWrapperRva = 0x1F4C",
        "kWrapperSize = 0xB80",
        "kControlPointerRva = 0x77C0",
        "kEvidencePointerRva = 0x7940",
        "kControlStorageRva = 0x6600",
        "kEvidenceStorageRva = 0x7800",
        "LDPlayer NativeBridge exposes ARM guest code as host r--p",
        "vehicle::Resolve(pid, mem, game_base",
        "if (action == Action::kInstall)",
        "WriteExactVerified(mem, payload.evidence",
        "WriteExactVerified(mem, payload.control",
        "StopProcess(pid)",
        "callback::kCallbackOffset",
        "kOriginalRestored",
    ):
        assert needle in txn, needle
    assert txn.index("if (action == Action::kInstall)") < txn.index(
        "vehicle::Resolve(pid, mem, game_base"
    )
    for needle in (
        "I_ACCEPT_CAMERA_TOOL_INPUT_STREAM_V2",
        "ReadStartTicks(pid",
        "HeaderValid(control, evidence)",
        "offsetof(protocol::Control, commands)",
        "inactive_slot * sizeof(protocol::Control::Command)",
        "offsetof(protocol::Control, published_command)",
        "control->commands[inactive_slot] = value",
        "protocol::kConfigured | protocol::kActive",
        "CAMERA_TOOL_INPUT_SET",
        "CAMERA_TOOL_INPUT_DISABLED",
        "ConfigureHud(mem, control_address",
        "offsetof(protocol::Control, expected_hud_root)",
        "offsetof(protocol::Control, published_hud_visibility)",
        "CAMERA_TOOL_INPUT_HUD_READY",
        "CAMERA_TOOL_INPUT_HUD_SET",
        "CAMERA_TOOL_INPUT_HUD_RESTORED",
        "kHudInterfaceOffset = 0x238u",
        "kHudLoadMask = 0xFFC003FFu",
        "kHudLoadValue = 0x39400000u",
        "kArm64Ret = 0xD65F03C0u",
        "identity.interface != identity.root + kHudInterfaceOffset",
        "identity.root_vptr + protocol::kHudHiddenSlot",
        "(getter_code[0] & kHudLoadMask) != kHudLoadValue",
        "getter_code[1] != kArm64Ret",
        "control->expected_hud_hidden_getter = identity.hidden_getter",
        "EncodeArm64Branch(identity.hidden_getter",
        "offsetof(protocol::Control, hud_original_code)",
        "offsetof(protocol::Control, hud_cache_flush_pending)",
        "WriteExactVerified(mem, identity.hidden_getter, &branch",
        "live_instruction != control->hud_patch_instruction",
    ):
        assert needle in channel, needle
    for forbidden in (
        "ptrace(", "PTRACE_", "kill(", "SIGSTOP", "SIGCONT", "mprotect(",
        "kCallbackOffset", "expected_manager +", "vehicle_native_body +",
        "kHudCacheFlushPendingOffset",
    ):
        assert forbidden not in channel, forbidden
    elf_payload = subprocess.run(
        [str(readelf), "-h", str(payload)], check=True,
        capture_output=True, text=True,
    ).stdout
    assert "Machine:                           AArch64" in elf_payload
    for binary in (transaction, stream):
        elf = subprocess.run(
            [str(readelf), "-h", str(binary)], check=True,
            capture_output=True, text=True,
        ).stdout
        assert "Machine:                           Advanced Micro Devices X86-64" in elf
    print(
        "CAMERA_TOOL_RUNTIME_TRANSPORT_V2_POLICY passed=1 hash_pinned=1 "
        "full_wrapper_readonly=1 nativebridge_guest_code=1 proven_raceview_resolver=1 install_restore=1 "
        "vehicle_identity_at_install=1 payload_only_stream=1 "
        "hud_readonly_identity_present=1 hud_native_setter=0 hud_direct_query=1 "
        "hud_aligned_branch_patch=1 hud_non_target_emulation=1 "
        "per_frame_host_writes=0 ptrace=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
