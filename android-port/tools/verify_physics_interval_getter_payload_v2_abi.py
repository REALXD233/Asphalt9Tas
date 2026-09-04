#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
from pathlib import Path


def function_body(text: str, name: str) -> str:
    match = re.search(
        rf"^[0-9a-f]+ <{re.escape(name)}>:\n(.*?)(?=^[0-9a-f]+ <|\Z)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise ValueError(f"missing function {name}")
    return match.group(1)


def require_order(body: str, fragments: tuple[str, ...]) -> None:
    cursor = 0
    for fragment in fragments:
        position = body.find(fragment, cursor)
        if position < 0:
            raise ValueError(f"missing/out-of-order instruction: {fragment}")
        cursor = position + len(fragment)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("disassembly", type=Path)
    args = parser.parse_args()
    text = args.disassembly.read_text(encoding="utf-8")
    try:
        entry = function_body(text, "PhysicsIntervalGetterEntryV2")
        require_order(entry, (
            "sub\tsp, sp, #0x160",
            "stp\tx19, x20, [sp]",
            "str\tx30, [sp, #0x10]",
            "mov\tx19, x0",
            "mov\tx20, x8",
            "adr\tx17,",
            "ldr\tx17, [x17]",
            "blr\tx17",
            "stp\tx0, x1, [sp, #0x20]",
            "stp\tx2, x3, [sp, #0x30]",
            "stp\tx4, x5, [sp, #0x40]",
            "stp\tx6, x7, [sp, #0x50]",
            "stp\tx8, x9, [sp, #0x60]",
            "stp\tx10, x11, [sp, #0x70]",
            "stp\tx12, x13, [sp, #0x80]",
            "stp\tx14, x15, [sp, #0x90]",
            "stp\tx16, x17, [sp, #0xa0]",
            "str\tx18, [sp, #0xb0]",
            "stp\tq0, q1, [sp, #0xc0]",
            "stp\tq2, q3, [sp, #0xe0]",
            "stp\tq4, q5, [sp, #0x100]",
            "stp\tq6, q7, [sp, #0x120]",
            "mrs\tx9, NZCV",
            "mrs\tx10, FPCR",
            "stp\tx9, x10, [sp, #0x140]",
            "mrs\tx9, FPSR",
            "str\tx9, [sp, #0x150]",
            "mov\tx0, x19",
            "mov\tx1, x20",
            "bl\t",
            "ldr\tx9, [sp, #0x150]",
            "msr\tFPSR, x9",
            "ldp\tx9, x10, [sp, #0x140]",
            "msr\tNZCV, x9",
            "msr\tFPCR, x10",
            "ldp\tq6, q7, [sp, #0x120]",
            "ldp\tq4, q5, [sp, #0x100]",
            "ldp\tq2, q3, [sp, #0xe0]",
            "ldp\tq0, q1, [sp, #0xc0]",
            "ldr\tx18, [sp, #0xb0]",
            "ldp\tx16, x17, [sp, #0xa0]",
            "ldp\tx14, x15, [sp, #0x90]",
            "ldp\tx12, x13, [sp, #0x80]",
            "ldp\tx10, x11, [sp, #0x70]",
            "ldp\tx8, x9, [sp, #0x60]",
            "ldp\tx6, x7, [sp, #0x50]",
            "ldp\tx4, x5, [sp, #0x40]",
            "ldp\tx2, x3, [sp, #0x30]",
            "ldp\tx0, x1, [sp, #0x20]",
            "ldr\tx30, [sp, #0x10]",
            "ldp\tx19, x20, [sp]",
            "add\tsp, sp, #0x160",
            "ret",
        ))
        if "PhysicsIntervalGetterAfterOriginalV2" not in entry:
            raise ValueError("post-original handler call not resolved")
        fake = function_body(text, "FakePhysicsIntervalGetterV2")
        require_order(fake, ("mov\tw9, #0x8889", "movk\tw9, #0x3c08", "str\tw9, [x8]", "ret"))
        invoke = function_body(text, "InvokePhysicsIntervalGetterV2")
        require_order(invoke, ("stp\tx29, x30", "mov\tx8, x1", "PhysicsIntervalGetterEntryV2", "ldp\tx29, x30", "ret"))
    except ValueError as exc:
        print(f"PHYSICS_INTERVAL_GETTER_PAYLOAD_V2_ABI_INVALID reason={exc}")
        return 1
    print("PHYSICS_INTERVAL_GETTER_PAYLOAD_V2_ABI_VALID original_before_handler=1 volatile_state_restored=1 x8_output_only=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
