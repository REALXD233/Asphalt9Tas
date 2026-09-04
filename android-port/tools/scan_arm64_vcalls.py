#!/usr/bin/env python3
"""Find AArch64 indirect virtual calls for a specific vtable slot.

The scanner intentionally reads the ELF only.  It looks for the common form:

    ldr xVtable, [xObject]
    ...
    ldr xTarget, [xVtable, #slot]
    ...
    blr xTarget

It is used to recover callers of stripped C++ virtual methods when IDA cannot
create a static xref from a vtable entry to an indirect BLR callsite.
"""

from __future__ import annotations

import argparse
import dataclasses
import struct
from pathlib import Path

import capstone


PT_LOAD = 1
PF_X = 1


@dataclasses.dataclass(frozen=True)
class LoadSegment:
    offset: int
    virtual_address: int
    file_size: int


def code_regions(image: bytes) -> list[LoadSegment]:
    if image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        raise ValueError("expected a little-endian ELF64 image")

    section_header_offset = struct.unpack_from("<Q", image, 40)[0]
    section_header_size = struct.unpack_from("<H", image, 58)[0]
    section_header_count = struct.unpack_from("<H", image, 60)[0]
    string_table_index = struct.unpack_from("<H", image, 62)[0]
    if (
        section_header_offset
        and section_header_size >= 64
        and section_header_count
        and string_table_index < section_header_count
    ):
        string_header = section_header_offset + string_table_index * section_header_size
        string_offset = struct.unpack_from("<Q", image, string_header + 24)[0]
        string_size = struct.unpack_from("<Q", image, string_header + 32)[0]
        strings = image[string_offset : string_offset + string_size]
        for index in range(section_header_count):
            cursor = section_header_offset + index * section_header_size
            if cursor + 64 > len(image):
                raise ValueError("section-header table exceeds the file")
            name_offset = struct.unpack_from("<I", image, cursor)[0]
            name_end = strings.find(b"\0", name_offset)
            name = strings[name_offset:name_end] if name_end >= 0 else b""
            if name == b".text":
                address = struct.unpack_from("<Q", image, cursor + 16)[0]
                offset = struct.unpack_from("<Q", image, cursor + 24)[0]
                size = struct.unpack_from("<Q", image, cursor + 32)[0]
                if offset + size > len(image):
                    raise ValueError(".text section exceeds the file")
                return [LoadSegment(offset, address, size)]

    # Fallback for stripped images without a section table.
    program_header_offset = struct.unpack_from("<Q", image, 32)[0]
    program_header_size = struct.unpack_from("<H", image, 54)[0]
    program_header_count = struct.unpack_from("<H", image, 56)[0]
    if program_header_size < 56:
        raise ValueError("invalid ELF64 program-header size")

    result: list[LoadSegment] = []
    for index in range(program_header_count):
        cursor = program_header_offset + index * program_header_size
        if cursor + 56 > len(image):
            raise ValueError("program-header table exceeds the file")
        p_type, p_flags = struct.unpack_from("<II", image, cursor)
        p_offset, p_vaddr = struct.unpack_from("<QQ", image, cursor + 8)
        p_filesz = struct.unpack_from("<Q", image, cursor + 32)[0]
        if p_type == PT_LOAD and (p_flags & PF_X) and p_filesz:
            if p_offset + p_filesz > len(image):
                raise ValueError("executable segment exceeds the file")
            result.append(LoadSegment(p_offset, p_vaddr, p_filesz))
    if not result:
        raise ValueError("ELF contains no executable PT_LOAD segment")
    return result


def scan_segment(
    disassembler: capstone.Cs,
    code: bytes,
    virtual_address: int,
    slot: int,
    window_size: int,
    require_vtable_load: bool,
) -> list[tuple[int, list[capstone.CsInsn]]]:
    matches: list[tuple[int, list[capstone.CsInsn]]] = []
    aligned_size = len(code) & ~3
    words = memoryview(code)[:aligned_size].cast("I")
    for index, word in enumerate(words):
        # LDR Xt, [Xn, #imm12 * 8] (64-bit unsigned-immediate form).
        if word & 0xFFC00000 != 0xF9400000:
            continue
        displacement = ((word >> 10) & 0xFFF) * 8
        if displacement != slot:
            continue
        target_register = word & 0x1F
        vtable_register = (word >> 5) & 0x1F

        if require_vtable_load:
            found_vtable_load = False
            for prior_index in range(max(0, index - window_size), index):
                prior = words[prior_index]
                if (
                    prior & 0xFFC00000 == 0xF9400000
                    and prior & 0x1F == vtable_register
                    and ((prior >> 10) & 0xFFF) == 0
                ):
                    found_vtable_load = True
                    break
            if not found_vtable_load:
                continue

        branch_index = None
        for candidate_index in range(
            index + 1, min(len(words), index + window_size + 1)
        ):
            candidate = words[candidate_index]
            fixed = candidate & 0xFFFFFC1F
            branch_register = (candidate >> 5) & 0x1F
            if fixed in (0xD63F0000, 0xD61F0000) and branch_register == target_register:
                branch_index = candidate_index
                break
        if branch_index is None:
            continue

        context_start = max(0, index - 6)
        context_end = min(len(words), branch_index + 5)
        context_bytes = code[context_start * 4 : context_end * 4]
        context_address = virtual_address + context_start * 4
        context = list(disassembler.disasm(context_bytes, context_address))
        matches.append((virtual_address + branch_index * 4, context))
    return matches


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("slot", type=lambda value: int(value, 0))
    parser.add_argument("--window", type=int, default=10)
    parser.add_argument(
        "--strict-vtable-load",
        action="store_true",
        help="require an earlier ldr of the vtable pointer from object+0",
    )
    parser.add_argument(
        "--addresses-only",
        action="store_true",
        help="print one matching indirect-branch address per line",
    )
    parser.add_argument(
        "--call-start",
        type=lambda value: int(value, 0),
        help="only report indirect branches at or above this virtual address",
    )
    parser.add_argument(
        "--call-end",
        type=lambda value: int(value, 0),
        help="only report indirect branches below this virtual address",
    )
    args = parser.parse_args()
    if args.slot < 0 or args.slot % 8:
        parser.error("slot must be a non-negative, 8-byte-aligned offset")
    if args.window < 3 or args.window > 64:
        parser.error("window must be between 3 and 64 instructions")

    image = args.elf.read_bytes()
    disassembler = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    disassembler.detail = True
    all_matches: list[tuple[int, list[capstone.CsInsn]]] = []
    segments = code_regions(image)
    for segment in segments:
        code = image[segment.offset : segment.offset + segment.file_size]
        all_matches.extend(
            scan_segment(
                disassembler,
                code,
                segment.virtual_address,
                args.slot,
                args.window,
                args.strict_vtable_load,
            )
        )

    if args.call_start is not None:
        all_matches = [match for match in all_matches if match[0] >= args.call_start]
    if args.call_end is not None:
        all_matches = [match for match in all_matches if match[0] < args.call_end]

    print(
        f"elf={args.elf} slot=0x{args.slot:x} "
        f"exec_segments={len(segments)} matches={len(all_matches)}"
    )
    for call_address, context in all_matches:
        if args.addresses_only:
            print(f"0x{call_address:x}")
            continue
        print(f"\nCALL 0x{call_address:x}")
        for instruction in context:
            print(
                f"  0x{instruction.address:x}: "
                f"{instruction.mnemonic} {instruction.op_str}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
