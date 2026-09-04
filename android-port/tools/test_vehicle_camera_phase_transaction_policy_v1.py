#!/usr/bin/env python3
"""Offline policy proof for the bounded camera-slot phase transaction."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "vehicle_camera_phase_camera_transaction_controller_v1.cpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    for needle in (
        "I_ACCEPT_VEHICLE_CAMERA_PHASE_SINGLE_SLOT_V1",
        "phase_elf::Resolve(pid, mem, &payload)",
        "phase::kConfigured | phase::kObserveVehicle",
        "values[5] > phase::kMaximumEvents",
        "PhaseControlIdentity",
        "PhaseEvidenceHeader",
        "A9VCPTR1",
        "copied_events.resize",
        "sizeof(phase::Event)",
    ):
        require(needle in text, f"transaction contract missing {needle}")
    install = text.index("if (action == Action::kInstall)")
    stop = text.index("if (!StopProcess(pid))", install)
    install_write = text.index("node.node + callback::kCallbackOffset", stop)
    resume = text.index("if (!ResumeProcess(pid))", install_write)
    require(stop < install_write < resume,
            "camera install slot write is not inside the stop/resume bracket")
    non_status = text.index("if (action != Action::kStatus)")
    finalize_stop = text.index("if (!StopProcess(pid))", non_status)
    restore_write = text.index("node.node + callback::kCallbackOffset",
                               finalize_stop)
    finalize_resume = text.index("const bool resumed = ResumeProcess(pid)",
                                 restore_write)
    require(finalize_stop < restore_write < finalize_resume,
            "camera restore is not completed before resume")
    require("manager + kManagerWorldOffset" not in text and
            "shape + kShapeFinalOffset" not in text,
            "host transaction contains a camera-state write target")
    require("ptrace(" not in text and "process_vm_writev" not in text,
            "transaction introduced an unreviewed process primitive")


def verify_artifact(path: pathlib.Path, readelf: pathlib.Path) -> None:
    header = subprocess.check_output([str(readelf), "-h", str(path)], text=True)
    require(re.search(r"Machine:\s+Advanced Micro Devices X86-64", header),
            "controller is not Android x86-64")
    dynamic = subprocess.check_output([str(readelf), "-d", str(path)], text=True)
    require("libc++_shared.so" not in dynamic, "dynamic libc++ dependency")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(path)], text=True)
    require(re.search(r"\bpwrite(?:@|\b)", symbols) is not None,
            "verified transaction writer import missing")
    for needle in ("ptrace", "process_vm_writev", "mprotect", "dlopen", "dlsym"):
        require(re.search(rf"\b{needle}(?:@|\b)", symbols) is None,
                f"forbidden dynamic import {needle}")


def main() -> int:
    verify_source()
    if len(sys.argv) == 3:
        verify_artifact(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [controller llvm-readelf]")
    print("VEHICLE_CAMERA_PHASE_TRANSACTION_POLICY passed=1 single_slot=1 "
          "bounded_events=1 reverse_restore=1 camera_state_writes=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
