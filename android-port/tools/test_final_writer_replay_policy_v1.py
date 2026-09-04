#!/usr/bin/env python3
"""Offline policy and ARM64 ABI proof for the finite final-writer payload."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_final_writer_replay_v1.cpp"
PROTOCOL = ROOT / "src" / "final_writer_replay_protocol_v1.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source_contract() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    protocol = PROTOCOL.read_text(encoding="utf-8")
    required = {
        "original callback": "kOriginalCallbackRva = 0x367D66C",
        "transform size": "kTransformSize = 64",
        "linear size": "kLinearSize = 12",
        "component count": "kComponentCount = 19",
        "finite bound": "kMaximumFrames = 3600",
        "disarmed frame permit": "kFramePermitDisarmed = 0",
        "indexed frame permit": "FramePermit(std::uint32_t frame_index)",
        "build-only rejection": "return kStatusRejectedBuildOnly;",
        "component equality": "current[index] != recorded[index]",
        "two correction writes": "g_evidence.correction_writes, 2u",
        "completion restore": "RestoreOriginalVptr(object, shadow_vptr, original_vptr)",
    }
    for label, needle in required.items():
        require(needle in text or needle in protocol, f"missing {label}")

    require('#include "final_writer_replay_protocol_v1.h"' in text,
            "payload does not consume shared protocol")
    for needle in ("sizeof(FrameTarget) == 80", "sizeof(FrameAudit) == 160",
                   "sizeof(Control) == 128", "sizeof(Evidence) == 192",
                   "offsetof(Control, recording_sha256) == 72",
                   "offsetof(Control, reserved) == 104",
                   "offsetof(Evidence, processed_frames) == 80"):
        require(needle in protocol, f"shared ABI proof missing: {needle}")

    forbidden = (
        "ptrace(", "process_vm_writev", "pwrite(", "mprotect(", "dlopen(",
        "pthread_create", "socket(", "connect(", "accept(", "send(",
        "recv(", "Nitro", "fixed_delta", "input keyevent",
    )
    for needle in forbidden:
        require(needle not in text and needle not in protocol,
                f"forbidden primitive: {needle}")

    wrapper = re.search(
        r"a9tas_final_writer_replay_callback_v1\(.*?\n}\n\nextern \"C\"",
        text,
        re.DOTALL,
    )
    require(wrapper is not None, "wrapper body not found")
    body = wrapper.group(0)
    permit_claim = body.index("__atomic_compare_exchange_n")
    unpermitted_original = body.index("return original(object, frame_token);")
    original_call = body.index("original(object, frame_token)")
    compare = body.index("ComponentsEqual(pose, linear, target)")
    pose_copy = body.index("CopyBytes(pose, target.transform")
    linear_copy = body.index("CopyBytes(linear, target.linear")
    immediate = body.index("audit.immediate_transform")
    processed = body.index("g_evidence.processed_frames, processed")
    require(original_call < compare < pose_copy < linear_copy < immediate < processed,
            "final-writer semantic order changed")
    require(permit_claim < unpermitted_original < compare,
            "unpermitted callbacks must call only the original writer")
    require("&g_control.reserved[0]" in body and
            "kFramePermitDisarmed" in body,
            "frame permit is not atomically claimed and disarmed")
    require(body.count("const std::int64_t result = original(object, frame_token);") == 1,
            "valid path must call original exactly once")
    require("return result;" in body, "original result is not propagated")


def verify_behavioral_model() -> None:
    def apply(current: list[float], target: list[float]) -> tuple[list[float], bool]:
        require(len(current) == 19 and len(target) == 19, "model payload size")
        equal = all(left == right for left, right in zip(current, target))
        return (current[:] if equal else target[:]), not equal

    target = [float(index) for index in range(19)]
    after, corrected = apply(target, target)
    require(after == target and not corrected, "equal frame wrote unexpectedly")
    current = target[:]
    current[18] += 1.0
    after, corrected = apply(current, target)
    require(after == target and corrected, "linear mismatch did not restore all 19")
    current = target[:]
    current[0] += 1.0
    after, corrected = apply(current, target)
    require(after == target and corrected, "transform mismatch did not restore all 19")
    signed_zero_current = target[:]
    signed_zero_target = target[:]
    signed_zero_current[0] = -0.0
    signed_zero_target[0] = 0.0
    _, corrected = apply(signed_zero_current, signed_zero_target)
    require(not corrected, "component comparison must treat signed zero as equal")

    processed = 0
    permit = 0

    def callback() -> bool:
        nonlocal processed, permit
        expected = processed + 1
        if permit != expected:
            return False
        permit = 0
        processed += 1
        return True

    require(not callback() and processed == 0,
            "unpermitted natural callback consumed a replay frame")
    permit = 1
    require(callback() and processed == 1 and permit == 0,
            "frame-zero permit was not claimed exactly once")
    require(not callback() and processed == 1,
            "one permit authorized more than one callback")
    permit = 2
    require(callback() and processed == 2 and permit == 0,
            "frame-one permit did not bind to the next cursor")


def verify_artifact(path: pathlib.Path, readelf: pathlib.Path,
                    objdump: pathlib.Path) -> None:
    require(path.is_file(), f"artifact missing: {path}")
    header = subprocess.check_output([str(readelf), "-h", str(path)], text=True)
    require(re.search(r"Machine:\s+AArch64", header) is not None,
            "artifact is not AArch64")
    dynamic = subprocess.check_output([str(readelf), "-d", str(path)], text=True)
    require("libc++_shared.so" not in dynamic, "dynamic libc++ dependency")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(path)], text=True
    )
    exports = (
        "a9tas_final_writer_replay_callback_v1",
        "a9tas_final_writer_replay_protocol_v1",
        "a9tas_final_writer_replay_arm_v1",
        "a9tas_final_writer_replay_shadow_storage_data_v1",
        "a9tas_final_writer_replay_control_storage_data_v1",
        "a9tas_final_writer_replay_target_storage_data_v1",
        "a9tas_final_writer_replay_audit_storage_data_v1",
        "a9tas_final_writer_replay_evidence_storage_data_v1",
        "a9tas_final_writer_replay_control_size_data_v1",
        "a9tas_final_writer_replay_target_size_data_v1",
        "a9tas_final_writer_replay_audit_size_data_v1",
        "a9tas_final_writer_replay_evidence_size_data_v1",
    )
    for name in exports:
        require(name in symbols, f"missing export: {name}")
    for name in ("ptrace", "process_vm_writev", "pwrite", "mprotect", "dlopen",
                 "pthread_create", "socket", "connect", "accept", "send", "recv"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", symbols) is None,
                f"forbidden dynamic import: {name}")

    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(path)], text=True
    )
    wrapper = re.search(
        r"<a9tas_final_writer_replay_callback_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(wrapper is not None, "wrapper disassembly missing")
    require(re.search(r"\bblr\s+x\d+", wrapper.group(1)) is not None,
            "wrapper has no indirect original callback call")
    arm = re.search(
        r"<a9tas_final_writer_replay_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(arm is not None and "#-0x64" in arm.group(1) and "\tret" in arm.group(1),
            "build-only arm entry is not constant -100")


def main() -> int:
    verify_source_contract()
    verify_behavioral_model()
    if len(sys.argv) == 4:
        verify_artifact(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]),
                        pathlib.Path(sys.argv[3]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [artifact llvm-readelf llvm-objdump]")
    print("FINAL_WRITER_REPLAY_POLICY passed=1 build_only=1 original_first=1 "
          "component_compare=19 correction=64+12 finite_frames=3600 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
