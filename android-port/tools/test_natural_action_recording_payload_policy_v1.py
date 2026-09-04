#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_natural_action_recording_v1.cpp"
PROTOCOL = ROOT / "src" / "natural_action_recording_protocol_v1.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require(len(sys.argv) == 4, "usage: policy PAYLOAD READELF OBJDUMP")
    payload, readelf, objdump = (pathlib.Path(value) for value in sys.argv[1:])
    source = SOURCE.read_text(encoding="utf-8")
    protocol = PROTOCOL.read_text(encoding="utf-8")
    for token in (
        "a9tas_natural_action_recording_wrapper_v1",
        "Store(&g_counts[index], before + 1u)",
        "IncrementSingleProducer(&g_evidence.counted_calls)",
        "original(service)",
        "kConfigured | kInstalled",
        "kFrameOverflow",
        "active_sequence",
    ):
        require(token in source or token in protocol, f"missing: {token}")
    require(source.index("Store(&g_counts[index], before + 1u)") <
            source.index("original(service)"),
            "real action must be counted before the original call")
    for forbidden_runtime in ("syscall(", "__atomic_fetch_add",
                              "__atomic_add_fetch"):
        require(forbidden_runtime not in source,
                f"wrapper runtime helper forbidden: {forbidden_runtime}")
    for forbidden in ("NitroState", "kNitroMode", "process_vm_writev",
                      "ptrace(", "input keyevent", "input tap", "socket("):
        require(forbidden not in source, f"forbidden primitive: {forbidden}")
    header = subprocess.check_output([str(readelf), "-h", str(payload)], text=True)
    require("AArch64" in header and re.search(r"Type:\s+DYN", header),
            "payload ELF identity")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(payload)], text=True)
    for name in (
        "a9tas_natural_action_recording_wrapper_v1",
        "a9tas_natural_action_recording_control_storage_data_v1",
        "a9tas_natural_action_recording_evidence_storage_data_v1",
        "a9tas_natural_action_recording_counts_storage_data_v1",
        "a9tas_natural_action_recording_shadow_storage_data_v1",
    ):
        require(name in symbols, f"missing dynamic symbol: {name}")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(payload)], text=True)
    require("a9tas_natural_action_recording_wrapper_v1" in disassembly,
            "wrapper optimized away")
    wrapper = disassembly.split(
        "<a9tas_natural_action_recording_wrapper_v1>:", 1)[1].split(
            "\n\n", 1)[0]
    require("@plt" not in wrapper and "\tbl\t" not in wrapper,
            "wrapper must not enter PLT or outlined runtime helpers")
    print("NATURAL_ACTION_RECORDING_PAYLOAD_POLICY passed=1 "
          "count_before_original=1 natural_game_call=1 colour_forcing=0 "
          "threads=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
