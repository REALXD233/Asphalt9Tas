#!/usr/bin/env python3
"""Offline source, ABI, and ARM64 policy proof for the phase observer."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_vehicle_camera_phase_observer_v1.cpp"
PROTOCOL = ROOT / "src" / "vehicle_camera_phase_observer_protocol_v1.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(text: str, name: str, next_marker: str) -> str:
    match = re.search(
        rf"{re.escape(name)}\(.*?\n}}\n\n{re.escape(next_marker)}",
        text,
        re.DOTALL,
    )
    require(match is not None, f"function body missing: {name}")
    return match.group(0)


def verify_source_contract() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    protocol = PROTOCOL.read_text(encoding="utf-8")

    for needle in (
        '#include "final_writer_replay_protocol_v1.h"',
        '#include "vehicle_camera_phase_observer_protocol_v1.h"',
        "SYS_clock_gettime",
        "CLOCK_MONOTONIC",
        "SYS_gettid",
        "kMaximumEvents = 16384",
        "sizeof(Event) == 80",
        "sizeof(Control) == 128",
        "sizeof(Evidence) == 192",
    ):
        require(needle in text or needle in protocol,
                f"required phase contract missing: {needle}")

    vehicle = function_body(
        text,
        "a9tas_vehicle_camera_phase_vehicle_callback_v1",
        'extern "C" __attribute__((noinline, visibility("default"))) void',
    )
    before = vehicle.index("phase::kVehicleBeforeFinalWriter")
    base_call = vehicle.index("a9tas_final_writer_replay_callback_v1(object")
    audit = vehicle.index("audits[frame_index].flags")
    after = vehicle.index("phase::kVehicleAfterFinalWriter")
    require(before < base_call < audit < after,
            "vehicle wrapper no longer brackets the immutable final writer")
    require(vehicle.count("a9tas_final_writer_replay_callback_v1(object") == 1,
            "vehicle wrapper must call the immutable final writer exactly once")
    require("frame_permit == final_writer::FramePermit(frame_index)" in vehicle,
            "vehicle phase bracket is not bound to the current frame permit")
    require("CopyBytes(pose" not in vehicle and "CopyBytes(linear" not in vehicle,
            "outer vehicle observer must not duplicate correction writes")

    camera = function_body(
        text,
        "a9tas_vehicle_camera_phase_camera_callback_v1",
        'extern "C" __attribute__((visibility("default"))) std::uint32_t',
    )
    original_call = camera.index(
        "reinterpret_cast<CameraCallback>(original_address)(manager)")
    manager_read = camera.index("expected_manager +\n                                          kManagerFovOffset")
    event = camera.index("phase::kCameraAfterOriginal")
    require(original_call < manager_read < event,
            "camera observation must happen after the original callback")
    require(camera.count("reinterpret_cast<CameraCallback>(original_address)(manager)") == 1,
            "camera wrapper must call the original exactly once")
    require("CopyBytes(&fov_bits" in camera,
            "camera FOV must be copied into local storage")
    require("phase::kVehicleSnapshotPresent" in camera and
            "HashBytes(vehicle_pose, final_writer::kTransformSize)" in camera and
            "HashBytes(vehicle_linear, final_writer::kLinearSize)" in camera,
            "camera wrapper must capture the read-only final-writer target")
    require("CopyBytes(reinterpret_cast<void*>" not in camera,
            "camera wrapper contains a remote-looking write destination")

    forbidden = (
        "ptrace(", "process_vm_writev", "pwrite(", "mprotect(", "dlopen(",
        "pthread_create", "socket(", "connect(", "accept(", "send(",
        "recv(", "input keyevent", "smooth", "interpolat",
    )
    for needle in forbidden:
        require(needle not in text.lower(), f"forbidden primitive: {needle}")
    require("return phase::kRejectedBuildOnly;" in text,
            "build-only arm entry no longer rejects execution")


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
        "a9tas_final_writer_replay_control_storage_data_v1",
        "a9tas_final_writer_replay_audit_storage_data_v1",
        "a9tas_final_writer_replay_evidence_storage_data_v1",
        "a9tas_vehicle_camera_phase_vehicle_callback_v1",
        "a9tas_vehicle_camera_phase_camera_callback_v1",
        "a9tas_vehicle_camera_phase_protocol_v1",
        "a9tas_vehicle_camera_phase_arm_v1",
        "a9tas_vehicle_camera_phase_control_storage_v1",
        "a9tas_vehicle_camera_phase_evidence_storage_v1",
        "a9tas_vehicle_camera_phase_events_storage_v1",
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
    vehicle = re.search(
        r"<a9tas_vehicle_camera_phase_vehicle_callback_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(vehicle is not None, "vehicle wrapper disassembly missing")
    require(re.search(r"\bbl\s+.*a9tas_final_writer_replay_callback_v1", vehicle.group(1))
            is not None,
            "vehicle wrapper does not call the immutable final writer")
    camera = re.search(
        r"<a9tas_vehicle_camera_phase_camera_callback_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(camera is not None, "camera wrapper disassembly missing")
    require(re.search(r"\bblr\s+x\d+", camera.group(1)) is not None,
            "camera wrapper has no indirect original callback call")
    arm = re.search(
        r"<a9tas_vehicle_camera_phase_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(arm is not None and "#-0x64" in arm.group(1) and "\tret" in arm.group(1),
            "build-only arm entry is not constant -100")


def main() -> int:
    verify_source_contract()
    if len(sys.argv) == 4:
        verify_artifact(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]),
                        pathlib.Path(sys.argv[3]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [artifact llvm-readelf llvm-objdump]")
    print("VEHICLE_CAMERA_PHASE_OBSERVER_POLICY passed=1 build_only=1 "
          "vehicle_write=0 camera_write=0 monotonic_clock=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
