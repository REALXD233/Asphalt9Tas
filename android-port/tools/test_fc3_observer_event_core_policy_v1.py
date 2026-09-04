#!/usr/bin/env python3
"""Offline policy/model checks for the FC-3 four-watchpoint event core."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc3_observer_event_core_v1.h"
SOURCE = ROOT / "src" / "fc3_observer_event_core_v1.cpp"
BUILD = ROOT / "build-fc3-observer-event-core-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for needle in (
        "kBeforeDedicated",
        "kAfterDedicated",
        "kRearmNitroAndContinue",
        "bool rejected_ = false",
        "DR0 callback flags, DR1 dedicated_entries, DR2 action queue end",
        "DR1 Nitro active/mode 8-byte block",
        "static_assert(sizeof(Stop) == 76",
    ):
        require(needle in header, f"missing FC-3 event-core token: {needle}")
    for needle in (
        "const std::uint32_t hits = stop.dr6 & 0xFu",
        "rejected_ = true",
        "(hits & (hits - 1u)) != 0",
        "if (hits == 0x4u)",
        "event.kind = protocol::EventKind::kDedicatedHit",
        "watch_plan_ = WatchPlan::kAfterDedicated",
        "event.kind = protocol::EventKind::kNitroActiveWrite",
        "event.kind = protocol::EventKind::kActionQueueWrite",
        "event.kind = protocol::EventKind::kNextAccumulator",
        "protocol::Phase::kWaitingCallbackClose",
        "protocol::Phase::kWaitingNextCallbackOpen",
        "FC3_EVENT_CORE_BUILD_ONLY runtime=disabled return=-100",
    ):
        require(needle in source, f"missing FC-3 classifier rule: {needle}")
    for needle in (
        "ptrace(", "pwrite(", "pread(", "/proc/", "process_vm_writev",
        "PTRACE_", "socket(", "CarPhysicsState_dispatch_action",
        "NitroState_handle_activation", "input keyevent", "input tap",
    ):
        require(needle not in header + source,
                f"forbidden FC-3 event-core primitive: {needle}")
    require("adb" not in build.lower(), "FC-3 event-core build must be offline")


def classify(plan: str, phase: str, hit: int, flags: int) -> tuple[str, str]:
    require(hit in (1, 2, 4, 8), "model accepts exactly one DR hit")
    if plan == "before":
        if hit == 2:
            return "after", "dedicated"
        if hit == 4:
            return "reject", "action"
        return "before", "ignore"
    if hit == 2:
        return "reject", "nitro"
    if hit == 4:
        return "reject", "action"
    if hit == 8:
        return ("after", "accumulator") if phase == "accumulator" else (
            "reject", "order")
    if phase == "close" and flags == 0:
        return "after", "close"
    if phase == "next_open" and flags == 1:
        return "proved", "next_open"
    return "reject", "order"


def verify_behavioral_model() -> None:
    require(classify("before", "dedicated", 1, 0) == ("before", "ignore"),
            "pre-gate callback ignored")
    require(classify("before", "dedicated", 8, 0) == ("before", "ignore"),
            "pre-gate accumulator ignored")
    require(classify("before", "dedicated", 4, 0) == ("reject", "action"),
            "pre-gate action never ignored")
    require(classify("before", "dedicated", 2, 1) == ("after", "dedicated"),
            "dedicated transition")
    require(classify("after", "close", 1, 0) == ("after", "close"),
            "callback close")
    require(classify("after", "accumulator", 8, 0) ==
            ("after", "accumulator"), "phase witness")
    require(classify("after", "next_open", 1, 1) ==
            ("proved", "next_open"), "next open")
    require(classify("after", "close", 2, 0) == ("reject", "nitro"),
            "Nitro mutation")
    require(classify("after", "close", 4, 0) == ("reject", "action"),
            "action mutation")


def verify_artifacts(passive: pathlib.Path, review: pathlib.Path,
                     readelf: pathlib.Path, objdump: pathlib.Path) -> None:
    for path in (passive, review, readelf, objdump):
        require(path.is_file(), f"missing FC-3 event-core artifact/tool: {path}")
    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(passive)], text=True
    )
    review_header = subprocess.check_output(
        [str(readelf), "-h", str(review)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header,
            "FC-3 event-core passive architecture")
    require("Advanced Micro Devices X86-64" in review_header and
            re.search(r"Type:\s+REL", review_header),
            "FC-3 event-core review must be x86-64 REL")
    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "open", "waitpid", "socket"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive FC-3 event core imports primitive: {name}")
    symbols = subprocess.check_output(
        [str(readelf), "--symbols", "--wide", str(review)], text=True
    )
    require("a9tas_fc3_event_core_review_protocol_v1" in symbols,
            "FC-3 event-core review export missing")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(review)], text=True
    )
    require("Core::Consume" in disassembly,
            "FC-3 event-core classifier missing from review object")


def main() -> int:
    verify_source()
    verify_behavioral_model()
    if len(sys.argv) == 5:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [passive review-object readelf objdump]"
        )
    print("FC3_EVENT_CORE_POLICY passed=1 dr_map=flags-dedicated-action-witness "
          "nitro_rearm=1 order_fail_closed=1 device_access=0 game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
