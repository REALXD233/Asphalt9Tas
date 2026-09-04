#!/usr/bin/env python3
"""Offline composition proof for the phase-aware final-writer executor."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "hwbp_vehicle_camera_phase_unified_replay_v1.cpp"
INTEGRATION = ROOT / "src" / "final_writer_unified_integration_v1.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    entry = ENTRY.read_text(encoding="utf-8")
    integration = INTEGRATION.read_text(encoding="utf-8")
    require('#include "vehicle_camera_phase_observer_elf_resolver_v1.h"' in entry,
            "diagnostic entry does not preload the phase resolver")
    require("#define final_writer_replay_elf_v1 "
            "vehicle_camera_phase_observer_elf_v1" in entry,
            "diagnostic entry does not redirect the resolver namespace")
    require('#include "hwbp_lifecycle_final_writer_replay_v1.cpp"' in entry,
            "diagnostic entry does not reuse the lifecycle-bound executor")
    require("final_writer_replay_elf_v1::Layout payload{}" in integration and
            "final_writer_replay_elf_v1::Resolve(pid, mem, &result.payload)" in
            integration,
            "production integration no longer has its original resolver form")
    require("vehicle_camera_phase_observer" not in integration,
            "production integration was contaminated by diagnostic code")


def verify_object(path: pathlib.Path, readelf: pathlib.Path,
                  objdump: pathlib.Path) -> None:
    header = subprocess.check_output([str(readelf), "-h", str(path)], text=True)
    require(re.search(r"Machine:\s+Advanced Micro Devices X86-64", header),
            "review object is not x86-64")
    require(re.search(r"Type:\s+REL", header), "review artifact is not relocatable")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(path)], text=True)
    require("a9tas_final_writer_unified_embedded_main_v1" in disassembly,
            "proven final-writer executor main is absent")
    strings = path.read_bytes()
    require(b"liba9tas_vehicle_camera_phase_observer_v1_build_only.so" in strings,
            "combined phase payload basename absent from review object")


def verify_candidate(path: pathlib.Path, readelf: pathlib.Path) -> None:
    header = subprocess.check_output([str(readelf), "-h", str(path)], text=True)
    require(re.search(r"Machine:\s+Advanced Micro Devices X86-64", header),
            "candidate is not Android x86-64")
    require(re.search(r"Type:\s+DYN", header), "candidate is not PIE/DYN")
    require(b"liba9tas_vehicle_camera_phase_observer_v1_build_only.so" in
            path.read_bytes(), "candidate lost the phase payload binding")


def main() -> int:
    verify_source()
    if len(sys.argv) == 5:
        verify_object(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]),
                      pathlib.Path(sys.argv[3]))
        verify_candidate(pathlib.Path(sys.argv[4]), pathlib.Path(sys.argv[2]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [review-object readelf objdump candidate]")
    print("VEHICLE_CAMERA_PHASE_UNIFIED_POLICY passed=1 proven_executor=1 "
          "outer_vehicle_wrapper=1 production_branch_preserved=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
