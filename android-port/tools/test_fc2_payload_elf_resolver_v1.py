#!/usr/bin/env python3
"""Offline proof for the no-guest-call FC-2 ARM64 ELF resolver."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc2_payload_elf_resolver_v1.h"
DEFAULT_PAYLOAD = (
    ROOT / "build" / "frame-callback-deferred-registration-v1" /
    "liba9tas_frame_callback_deferred_registration_v1_build_only.so"
)

NAMES = {
    "a9tas_fc2_frame_callback_bootstrap_v1": (2, 24),
    "a9tas_fc2_dedicated_observer_v1": (2, 24),
    "a9tas_fc2_shadow_storage_data_v1": (1, 8),
    "a9tas_fc2_control_storage_data_v1": (1, 8),
    "a9tas_fc2_evidence_storage_data_v1": (1, 8),
    "a9tas_fc2_dedicated_object_data_v1": (1, 8),
    "a9tas_fc2_dedicated_vtable_data_v1": (1, 8),
    "a9tas_fc2_control_size_data_v1": (1, 8),
    "a9tas_fc2_evidence_size_data_v1": (1, 8),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def c_expected_hash(text: str) -> bytes:
    block = re.search(
        r"kExpectedSha256\[32\]\s*=\s*\{(.*?)\};", text, re.DOTALL
    )
    require(block is not None, "embedded SHA-256 missing")
    value = bytes(
        int(item, 16)
        for item in re.findall(r"0x([0-9a-fA-F]{2})", block.group(1))
    )
    require(len(value) == 32, "embedded SHA-256 length")
    return value


def parse_elf(path: pathlib.Path):
    data = path.read_bytes()
    require(len(data) >= 64, "short ELF")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    ident, elf_type, machine = header[0], header[1], header[2]
    phoff, shoff = header[5], header[6]
    phentsize, phnum, shentsize, shnum = (
        header[9], header[10], header[11], header[12]
    )
    require(ident[:4] == b"\x7fELF" and ident[4] == 2 and ident[5] == 1,
            "not little-endian ELF64")
    require(elf_type == 3 and machine == 183, "not AArch64 ET_DYN")
    require(phentsize == 56 and shentsize == 64 and 0 < shnum <= 256,
            "ELF table ABI")
    require(phoff + phnum * phentsize <= len(data),
            "program table out of bounds")
    require(shoff + shnum * shentsize <= len(data),
            "section table out of bounds")

    loads = []
    for index in range(phnum):
        item = struct.unpack_from("<IIQQQQQQ", data,
                                  phoff + index * phentsize)
        p_type, flags, offset, vaddr, _, filesz, memsz, _ = item
        if p_type == 1:
            loads.append((offset, vaddr, filesz, memsz, flags))

    sections = [
        struct.unpack_from("<IIQQQQIIQQ", data,
                           shoff + index * shentsize)
        for index in range(shnum)
    ]
    dynsyms = [section for section in sections if section[1] == 11]
    require(len(dynsyms) == 1, "required one DYNSYM")
    dynsym = dynsyms[0]
    require(dynsym[9] == 24 and dynsym[5] % 24 == 0,
            "DYNSYM entry ABI")
    require(dynsym[6] < len(sections), "DYNSYM string link")
    strings_section = sections[dynsym[6]]
    require(strings_section[1] == 3, "DYNSYM link is not STRTAB")
    strings = data[
        strings_section[4]:strings_section[4] + strings_section[5]
    ]
    require(len(strings) == strings_section[5],
            "short dynamic string table")

    found = {}
    symbol_indices = {}
    for index, offset in enumerate(range(
        dynsym[4], dynsym[4] + dynsym[5], 24
    )):
        name_offset, info, _, section_index, value, size = struct.unpack_from(
            "<IBBHQQ", data, offset
        )
        if name_offset >= len(strings) or section_index == 0:
            continue
        end = strings.find(b"\0", name_offset)
        require(end >= 0, "unterminated dynamic symbol")
        name = strings[name_offset:end].decode("ascii")
        if name not in NAMES:
            continue
        require(name not in found, f"duplicate export: {name}")
        require(info >> 4 == 1, f"non-global export: {name}")
        expected_type, minimum_size = NAMES[name]
        require(info & 0xF == expected_type and size >= minimum_size,
                f"export type/size: {name}")
        found[name] = (value, size, section_index)
        symbol_indices[name] = index
    require(set(found) == set(NAMES), "missing required dynamic exports")

    relocations = {}
    for section in sections:
        if section[1] != 4:
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "RELA entry ABI")
        for offset in range(section[4], section[4] + section[5], 24):
            target, info, addend = struct.unpack_from("<QQq", data, offset)
            require(target not in relocations,
                    f"duplicate relocation target: 0x{target:x}")
            relocations[target] = (
                info & 0xFFFFFFFF, info >> 32, addend
            )
    return data, found, symbol_indices, relocations, loads


def vaddr_bytes(data: bytes, loads, address: int, size: int) -> bytes:
    for offset, vaddr, filesz, _, _ in loads:
        if vaddr <= address and address + size <= vaddr + filesz:
            start = offset + address - vaddr
            return data[start:start + size]
    raise AssertionError(f"virtual address not file-backed: 0x{address:x}")


def verify_source_policy(text: str) -> None:
    required = (
        "ReadPayloadMappings", "ResolveSymbols", "SHT_DYNSYM",
        "SHT_RELA", "R_AARCH64_RELATIVE", "R_AARCH64_ABS64",
        "observer_slot.symbol != observer_symbol_index",
        "object_vptr.addend !=",
        "runtime_object_vptr != layout.dedicated_vptr",
        "runtime_slots[2] != layout.dedicated_observer",
        "runtime_prefix[0] != 0 || runtime_prefix[1] != 0",
        "layout.control_size != 128", "layout.evidence_size != 256",
        "common::HashFile", "kExpectedSha256",
        "mapping.begin == expected_begin",
        "in_file_load(runtime_rva(output->dedicated_observer), 24",
        "common::ReadAt(process_mem",
    )
    for needle in required:
        require(needle in text, f"resolver policy missing: {needle}")
    for needle in (
        "pwrite", "ptrace", "dlopen", "dlsym", "NativeBridge",
        "process_vm_writev", "PTRACE_", "mprotect",
    ):
        require(needle not in text, f"resolver forbidden primitive: {needle}")


def main() -> int:
    payload = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_PAYLOAD
    if len(sys.argv) > 2:
        raise SystemExit(f"usage: {sys.argv[0]} [payload]")
    header_text = HEADER.read_text(encoding="utf-8")
    verify_source_policy(header_text)
    data, symbols, indices, relocations, loads = parse_elf(payload)
    require(hashlib.sha256(data).digest() == c_expected_hash(header_text),
            "payload SHA-256 differs from resolver pin")

    pointer_names = (
        "a9tas_fc2_shadow_storage_data_v1",
        "a9tas_fc2_control_storage_data_v1",
        "a9tas_fc2_evidence_storage_data_v1",
        "a9tas_fc2_dedicated_object_data_v1",
        "a9tas_fc2_dedicated_vtable_data_v1",
    )
    targets = {}
    for name in pointer_names:
        locator = symbols[name][0]
        require(relocations.get(locator, (0, 0, 0))[:2] == (1027, 0),
                f"missing AArch64 RELATIVE locator: {name}")
        targets[name] = relocations[locator][2]

    object_rva = targets["a9tas_fc2_dedicated_object_data_v1"]
    vptr_rva = targets["a9tas_fc2_dedicated_vtable_data_v1"]
    require(relocations.get(object_rva) == (1027, 0, vptr_rva),
            "dedicated object vptr relocation")
    slot_zero = relocations.get(vptr_rva)
    slot_eight = relocations.get(vptr_rva + 8)
    require(slot_zero is not None and slot_zero[0:2] == (1027, 0),
            "dedicated fail-safe slot +0 relocation")
    require(slot_eight == slot_zero,
            "dedicated fail-safe slots must match")
    require(relocations.get(vptr_rva + 16) == (
        257, indices["a9tas_fc2_dedicated_observer_v1"], 0
    ), "dedicated +0x10 observer relocation")
    require(vaddr_bytes(data, loads, vptr_rva - 16, 16) == b"\0" * 16,
            "Itanium vtable prefix is not zero")

    for name in (
        "a9tas_fc2_frame_callback_bootstrap_v1",
        "a9tas_fc2_dedicated_observer_v1",
    ):
        address = symbols[name][0]
        require(any(
            (flags & 0x5) == 0x5 and vaddr <= address and
            address + 24 <= vaddr + filesz
            for _, vaddr, filesz, _, flags in loads
        ), f"FC-2 code export lacks file-backed PF_R|PF_X: {name}")
    fail_safe = slot_zero[2]
    require(any(
        (flags & 0x5) == 0x5 and vaddr <= fail_safe < vaddr + filesz
        for _, vaddr, filesz, _, flags in loads
    ), "fail-safe callback lacks file-backed PF_R|PF_X")
    require(struct.unpack("<Q", vaddr_bytes(
        data, loads, symbols["a9tas_fc2_control_size_data_v1"][0], 8
    ))[0] == 128, "control-size ABI")
    require(struct.unpack("<Q", vaddr_bytes(
        data, loads, symbols["a9tas_fc2_evidence_size_data_v1"][0], 8
    ))[0] == 256, "evidence-size ABI")
    print("FC2_ELF_RESOLVER passed=1 sha256=1 elf64=1 aarch64=1 "
          "dynsym=9 relative_locators=5 object_vptr=1 dedicated_vslot=1 "
          "itanium_prefix=1 guest_calls=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
