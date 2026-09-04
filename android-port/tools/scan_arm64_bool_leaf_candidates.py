#!/usr/bin/env python3
"""Find small AArch64 boolean-return leaf functions and their references.

This is an offline-only narrowing tool for stripped Android game builds.  It
does not assume that a candidate is a GUI gate; it merely reports compact leaf
functions that write W0 and return, together with direct BL callsites and
aligned pointers from non-executable PT_LOAD segments.  The latter matter for
virtual functions, which may have no direct code xrefs at all.  Candidates can
then be qualified by their caller/vtable graph before any live hook is
attempted.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import struct
from pathlib import Path

import capstone


PT_LOAD = 1
PF_X = 1


@dataclasses.dataclass(frozen=True)
class Region:
    offset: int
    address: int
    size: int
    flags: int


def load_regions(image: bytes) -> list[Region]:
    if image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        raise ValueError("expected a little-endian ELF64 image")
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize = struct.unpack_from("<H", image, 54)[0]
    phnum = struct.unpack_from("<H", image, 56)[0]
    if phentsize < 56:
        raise ValueError("invalid ELF64 program-header size")
    result: list[Region] = []
    for index in range(phnum):
        cursor = phoff + index * phentsize
        p_type, p_flags = struct.unpack_from("<II", image, cursor)
        p_offset, p_vaddr = struct.unpack_from("<QQ", image, cursor + 8)
        p_filesz = struct.unpack_from("<Q", image, cursor + 32)[0]
        if p_type == PT_LOAD and p_filesz:
            if p_offset + p_filesz > len(image):
                raise ValueError("PT_LOAD exceeds file")
            result.append(Region(p_offset, p_vaddr, p_filesz, p_flags))
    if not result:
        raise ValueError("no PT_LOAD")
    return result


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def writes_w0(insn: capstone.CsInsn) -> bool:
    if not insn.operands or insn.operands[0].type != capstone.arm64.ARM64_OP_REG:
        return False
    return insn.reg_name(insn.operands[0].reg) == "w0"


def looks_boolean_result(block: list[capstone.CsInsn]) -> bool:
    writers = [insn for insn in block[:-1] if writes_w0(insn)]
    if not writers:
        return False
    writer = writers[-1]
    if writer.mnemonic in {"cset", "csetm", "ldrb", "ldarb"}:
        return True
    if writer.mnemonic == "mov" and len(writer.operands) >= 2:
        operand = writer.operands[1]
        return (
            operand.type == capstone.arm64.ARM64_OP_IMM
            and operand.imm in (0, 1)
        )
    if writer.mnemonic in {"and", "ands"} and len(writer.operands) >= 3:
        operand = writer.operands[2]
        return (
            operand.type == capstone.arm64.ARM64_OP_IMM
            and operand.imm == 1
        )
    return False


def is_control_boundary(insn: capstone.CsInsn) -> bool:
    return bool(
        insn.group(capstone.CS_GRP_JUMP)
        or insn.group(capstone.CS_GRP_CALL)
        or insn.group(capstone.CS_GRP_RET)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--max-insns", type=int, default=10)
    parser.add_argument("--min-caller", type=lambda value: int(value, 0))
    parser.add_argument("--max-caller", type=lambda value: int(value, 0))
    parser.add_argument("--min-data-ref", type=lambda value: int(value, 0))
    parser.add_argument("--max-data-ref", type=lambda value: int(value, 0))
    parser.add_argument(
        "--require-data-ref",
        action="store_true",
        help="only report candidates referenced by aligned non-executable data",
    )
    parser.add_argument("--limit", type=int, default=300)
    parser.add_argument(
        "--callers-of",
        action="append",
        type=lambda value: int(value, 0),
        help="print direct BL callers of an address and exit (repeatable)",
    )
    args = parser.parse_args()
    if args.max_insns < 2 or args.max_insns > 32:
        parser.error("--max-insns must be between 2 and 32")

    image = args.elf.read_bytes()
    loads = load_regions(image)
    regions = [region for region in loads if region.flags & PF_X]
    if not regions:
        raise ValueError("no executable PT_LOAD")
    direct_callers: dict[int, list[int]] = collections.defaultdict(list)
    direct_branches: dict[int, list[int]] = collections.defaultdict(list)
    region_words: list[tuple[Region, memoryview]] = []
    for region in regions:
        code = image[region.offset : region.offset + region.size]
        words = memoryview(code[: len(code) & ~3]).cast("I")
        region_words.append((region, words))
        for index, word in enumerate(words):
            if word & 0xFC000000 != 0x94000000:  # BL imm26
                if word & 0xFC000000 != 0x14000000:  # B imm26
                    continue
                branchsite = region.address + index * 4
                displacement = sign_extend(word & 0x03FFFFFF, 26) << 2
                direct_branches[branchsite + displacement].append(branchsite)
            else:
                callsite = region.address + index * 4
                displacement = sign_extend(word & 0x03FFFFFF, 26) << 2
                direct_callers[callsite + displacement].append(callsite)

    if args.callers_of:
        for target in args.callers_of:
            callers = direct_callers.get(target, [])
            branches = direct_branches.get(target, [])
            print(
                f"TARGET 0x{target:x} callers={len(callers)} "
                + ",".join(f"0x{value:x}" for value in callers)
            )
            print(
                f"TARGET 0x{target:x} tail_branches={len(branches)} "
                + ",".join(f"0x{value:x}" for value in branches)
            )
        return 0

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    raw_candidates: dict[int, list[capstone.CsInsn]] = {}
    ret_word = 0xD65F03C0
    for region, words in region_words:
        code = image[region.offset : region.offset + region.size]
        for ret_index, word in enumerate(words):
            if word != ret_word:
                continue
            earliest = max(0, ret_index - args.max_insns + 1)
            # Stop after the nearest earlier control-flow instruction.  This
            # keeps the reported slice within one compact terminal block.
            start = earliest
            decoded = list(
                md.disasm(
                    code[earliest * 4 : (ret_index + 1) * 4],
                    region.address + earliest * 4,
                )
            )
            for offset, insn in enumerate(decoded[:-1]):
                if is_control_boundary(insn):
                    start = earliest + offset + 1
            block = list(
                md.disasm(
                    code[start * 4 : (ret_index + 1) * 4],
                    region.address + start * 4,
                )
            )
            if not block or not looks_boolean_result(block):
                continue
            entry = block[0].address
            raw_candidates.setdefault(entry, block)

    # A compact terminal block with no BL xrefs can still be a real virtual
    # function.  Build an aligned pointer index over non-executable PT_LOADs so
    # such entries are retained only when the ELF itself publishes them.
    data_refs: dict[int, list[int]] = collections.defaultdict(list)
    candidate_entries = set(raw_candidates)
    for region in loads:
        if region.flags & PF_X:
            continue
        data = image[region.offset : region.offset + region.size]
        for offset in range(0, len(data) & ~7, 8):
            value = struct.unpack_from("<Q", data, offset)[0]
            if value in candidate_entries:
                data_refs[value].append(region.address + offset)

    candidates: list[
        tuple[int, list[int], list[int], list[capstone.CsInsn]]
    ] = []
    for entry, block in raw_candidates.items():
        callers = direct_callers.get(entry, [])
        if args.min_caller is not None:
            callers = [value for value in callers if value >= args.min_caller]
        if args.max_caller is not None:
            callers = [value for value in callers if value < args.max_caller]
        refs = data_refs.get(entry, [])
        if args.min_data_ref is not None:
            refs = [value for value in refs if value >= args.min_data_ref]
        if args.max_data_ref is not None:
            refs = [value for value in refs if value < args.max_data_ref]
        if args.require_data_ref and not refs:
            continue
        if not callers and not refs:
            continue
        candidates.append((entry, callers, refs, block))

    candidates.sort(key=lambda item: (-len(item[2]), -len(item[1]), item[0]))
    print(
        f"elf={args.elf} exec_regions={len(regions)} "
        f"direct_targets={len(direct_callers)} candidates={len(candidates)}"
    )
    for entry, callers, refs, block in candidates[: args.limit]:
        print(
            f"\nCANDIDATE 0x{entry:x} callers={len(callers)} "
            + ",".join(f"0x{value:x}" for value in callers[:16])
        )
        print(
            f"  data_refs={len(refs)} "
            + ",".join(f"0x{value:x}" for value in refs[:16])
        )
        for insn in block:
            print(f"  0x{insn.address:x}: {insn.mnemonic} {insn.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
