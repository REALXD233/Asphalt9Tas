#!/usr/bin/env python3
"""Strict offline audit of the P1 ARM64 hook-entry disassembly."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def symbol_block(text: str, symbol: str) -> str:
    match = re.search(
        rf"^[0-9a-f]+ <{re.escape(symbol)}>:\n(.*?)(?=^[0-9a-f]+ <|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise ValueError(f"missing disassembly symbol {symbol}")
    return match.group(1).lower()


def require(block: str, pattern: str, label: str) -> None:
    if not re.search(pattern, block):
        raise ValueError(f"missing {label}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disassembly", type=Path)
    parser.add_argument(
        "--expect-live-candidate",
        action="store_true",
        help="require the explicit one-shot InstallInternal arm path",
    )
    args = parser.parse_args()
    text = args.disassembly.read_text(encoding="utf-8")
    entry = symbol_block(text, "PhysicsExecutorAffinityEntryV1")
    arm = symbol_block(text, "a9tas_physics_executor_affinity_v1_arm")
    on_load = symbol_block(text, "(anonymous namespace)::OnLoad()")

    require(entry, r"sub\s+sp,\s*sp,\s*#0x160", "private 0x160 frame")
    require(entry, r"stp\s+x0,\s*x1,\s*\[sp", "X0/X1 save")
    require(entry, r"stp\s+x16,\s*x17,\s*\[sp", "X16/X17 save")
    require(entry, r"stp\s+x18,\s*x30,\s*\[sp", "X18/LR save")
    for pair in ((0, 1), (2, 3), (4, 5), (6, 7)):
        require(entry, rf"stp\s+q{pair[0]},\s*q{pair[1]},\s*\[sp",
                f"Q{pair[0]}/Q{pair[1]} save")
        require(entry, rf"ldp\s+q{pair[0]},\s*q{pair[1]},\s*\[sp",
                f"Q{pair[0]}/Q{pair[1]} restore")
    for register in ("nzcv", "fpcr", "fpsr"):
        require(entry, rf"mrs\s+x\d+,\s*{register}", f"{register} save")
        require(entry, rf"msr\s+{register},\s*x\d+", f"{register} restore")
    require(entry, r"bl\s+.*physicsexecutoraffinitycapturev1",
            "capture call")
    require(entry, r"add\s+sp,\s*sp,\s*#0x160", "private frame restore")
    require(entry, r"br\s+x17", "continuation tail branch")
    if re.search(r"sub\s+sp,\s*sp,\s*#0x60", entry):
        raise ValueError("hook entry replays the original 0x60 prologue")
    if len(re.findall(r"\bbr\s+x17", entry)) != 1:
        raise ValueError("hook entry must contain exactly one continuation branch")

    require(arm, r"ret", "arm return")
    if args.expect_live_candidate:
        require(arm, r"bl\s+.*installinternal", "live InstallInternal call")
        if not (
            re.search(r"(?:ldaxr|ldxr)", arm)
            and re.search(r"(?:stlxr|stxr)", arm)
        ) and not re.search(r"bl\s+.*__aarch64_cas[14]_acq_rel", arm):
            raise ValueError("missing one-shot atomic compare-exchange")
        if len(re.findall(r"bl\s+.*__aarch64_cas[14]_acq_rel", arm)) < 4:
            raise ValueError("missing split-address preflight/arm phase gates")
        require(arm, r"lsr\s+x0,\s*x\d+,\s*#32",
                "high 32-bit control-address return")
        require(arm, r"bl\s+.*probetargetprotection",
                "zero-write target-permission preflight")
        if re.search(r"mov\s+[wx]0,\s*#(?:0xffffff9c|-0x64)", arm):
            raise ValueError("live arm export contains build-only -100 result")
        install = symbol_block(
            text, "(anonymous namespace)::InstallInternal()"
        )
        atomic_patch = symbol_block(
            text,
            "(anonymous namespace)::AtomicSwapInstruction(unsigned long, "
            "unsigned int, unsigned int, int)",
        )
        permission_probe = symbol_block(
            text, "(anonymous namespace)::ProbeTargetProtection()"
        )
        if len(re.findall(r"bl\s+.*mprotect", permission_probe)) != 2:
            raise ValueError(
                "permission preflight must enter and restore protection exactly once"
            )
        if re.search(r"bl\s+.*pwrite", permission_probe):
            raise ValueError("permission preflight contains a target write")
        require(install, r"bl\s+.*buildnearbridge", "near-bridge build")
        require(
            install,
            r"bl\s+.*atomicswapinstruction",
            "single-instruction patch call",
        )
        require(atomic_patch, r"stlr\s+w\d+,\s*\[x\d+\]",
                "aligned 32-bit release store")
        require(atomic_patch, r"bl\s+.*__clear_cache", "instruction-cache flush")
        if len(re.findall(r"bl\s+.*mprotect", atomic_patch)) != 2:
            raise ValueError(
                "atomic patch must enter and restore text protection exactly once"
            )
    else:
        require(arm, r"mov\s+[wx]0,\s*#(?:0xffffff9c|-0x64)",
                "constant -100 arm result")
        if re.search(r"\bbl\b", arm):
            raise ValueError(
                "build-only arm export unexpectedly calls another function"
            )

    for forbidden in (
            "pthread_create", "mprotect", "pwrite", "mmap",
            "installinternal", "physicsexecutoraffinityentryv1"):
        if forbidden in on_load:
            raise ValueError(f"passive constructor references {forbidden}")

    mode = "live_candidate" if args.expect_live_candidate else "build_only"
    arm_result = (
        "split_address_permission_preflight_atomic_branch_install"
        if args.expect_live_candidate else "-100"
    )
    print(f"physics_executor_affinity_v1_audit=pass mode={mode} "
          f"arm={arm_result} single_original_prologue=1 "
          "volatile_state_preserved=1 constructor_passive=1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"physics_executor_affinity_v1_audit=fail error={error}")
        raise SystemExit(1)
