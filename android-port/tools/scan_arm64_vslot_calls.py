#!/usr/bin/env python3
"""Find AArch64 indirect calls through one virtual-table byte offset.

This is an offline evidence tool for stripped ELF64 Android builds.  It looks
for ``ldr Xt, [Xn, #slot]`` followed by a nearby ``blr Xt``/``br Xt`` without
claiming that every syntactic match is a C++ virtual call.  The surrounding
instructions are printed so each candidate can be qualified manually.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import capstone
from capstone import arm64


PT_LOAD = 1
PF_X = 1


def executable_regions(image: bytes) -> list[tuple[int, int, int]]:
    if image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        raise ValueError("expected little-endian ELF64")
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize = struct.unpack_from("<H", image, 54)[0]
    phnum = struct.unpack_from("<H", image, 56)[0]
    result: list[tuple[int, int, int]] = []
    for index in range(phnum):
        cursor = phoff + index * phentsize
        p_type, p_flags = struct.unpack_from("<II", image, cursor)
        p_offset, p_vaddr = struct.unpack_from("<QQ", image, cursor + 8)
        p_filesz = struct.unpack_from("<Q", image, cursor + 32)[0]
        if p_type == PT_LOAD and p_flags & PF_X and p_filesz:
            result.append((p_offset, p_vaddr, p_filesz))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--slot", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--lookahead", type=int, default=5)
    parser.add_argument("--context", type=int, default=5)
    parser.add_argument("--limit", type=int, default=500)
    parser.add_argument("--min-address", type=lambda value: int(value, 0))
    parser.add_argument("--max-address", type=lambda value: int(value, 0))
    parser.add_argument(
        "--allow-no-call",
        action="store_true",
        help="also print slot loads without a nearby call through the loaded register",
    )
    args = parser.parse_args()
    if args.lookahead < 1 or args.lookahead > 16:
        parser.error("--lookahead must be between 1 and 16")
    if args.context < 0 or args.context > 20:
        parser.error("--context must be between 0 and 20")

    image = args.elf.read_bytes()
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    # Executable PT_LOADs also contain ELF headers/PLT padding.  Without
    # skipdata Capstone stops at the first non-instruction and silently misses
    # the real .text region later in the segment.
    md.skipdata = True
    matches: list[tuple[int, int | None]] = []
    regions = executable_regions(image)
    for file_offset, address, size in regions:
        scan_start = max(address, args.min_address or address)
        scan_stop = min(address + size, args.max_address or address + size)
        if scan_start >= scan_stop:
            continue
        scan_offset = file_offset + scan_start - address
        code = image[scan_offset : scan_offset + scan_stop - scan_start]
        pending: list[tuple[int, int, int]] = []
        for insn in md.disasm(code[: len(code) & ~3], scan_start):
            next_pending: list[tuple[int, int, int]] = []
            for load_address, target_reg, remaining in pending:
                is_call = (
                    insn.mnemonic in {"blr", "br"}
                    and len(insn.operands) == 1
                    and insn.operands[0].type == arm64.ARM64_OP_REG
                    and insn.operands[0].reg == target_reg
                )
                if is_call:
                    matches.append((load_address, insn.address))
                elif remaining > 1:
                    next_pending.append((load_address, target_reg, remaining - 1))
                elif args.allow_no_call:
                    matches.append((load_address, None))
            pending = next_pending
            if insn.mnemonic != "ldr" or len(insn.operands) != 2:
                continue
            destination, source = insn.operands
            if destination.type != arm64.ARM64_OP_REG:
                continue
            if source.type != arm64.ARM64_OP_MEM or source.mem.disp != args.slot:
                continue
            pending.append((insn.address, destination.reg, args.lookahead))
        if args.allow_no_call:
            matches.extend((load_address, None) for load_address, _, _ in pending)

    print(f"elf={args.elf} slot=0x{args.slot:x} matches={len(matches)}")
    for load_address, call_address in matches[: args.limit]:
        call_text = "none" if call_address is None else f"0x{call_address:x}"
        print(f"\nMATCH load=0x{load_address:x} call={call_text}")
        region = next(
            item for item in regions if item[1] <= load_address < item[1] + item[2]
        )
        file_offset, address, size = region
        end_address = call_address or load_address
        start_address = max(address, load_address - args.context * 4)
        stop_address = min(address + size, end_address + (args.context + 1) * 4)
        start_offset = file_offset + start_address - address
        block = image[start_offset : start_offset + stop_address - start_address]
        for insn in md.disasm(block, start_address):
            marker = "*" if insn.address in {load_address, call_address} else " "
            print(f"{marker} 0x{insn.address:x}: {insn.mnemonic} {insn.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
