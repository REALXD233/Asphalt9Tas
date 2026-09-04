#!/usr/bin/env python3
"""Offline proof for the no-guest-call lifecycle payload ELF resolver."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "natural_action_lifecycle_elf_resolver_v1.h"
DEFAULT_PAYLOAD = (
    ROOT / "build" / "natural-action-callback-lifecycle-v1" /
    "liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
)

NAMES = {
    "a9tas_natural_action_registration_bootstrap_v1": (2, 24),
    "a9tas_natural_action_persistent_consumer_v1": (2, 24),
    "a9tas_natural_action_lifecycle_shadow_data_v1": (1, 8),
    "a9tas_natural_action_lifecycle_control_data_v1": (1, 8),
    "a9tas_natural_action_lifecycle_evidence_data_v1": (1, 8),
    "a9tas_natural_action_lifecycle_mailbox_data_v1": (1, 8),
    "a9tas_natural_action_lifecycle_object_data_v1": (1, 8),
    "a9tas_natural_action_lifecycle_vtable_data_v1": (1, 8),
    "a9tas_natural_action_lifecycle_control_size_v1": (1, 8),
    "a9tas_natural_action_lifecycle_evidence_size_v1": (1, 8),
    "a9tas_natural_action_lifecycle_mailbox_size_v1": (1, 8),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def c_bytes(text: str, name: str, size: int) -> bytes:
    block = re.search(
        rf"{re.escape(name)}\[{size}\]\s*=\s*\{{(.*?)\}};",
        text,
        re.DOTALL,
    )
    require(block is not None, f"embedded bytes missing: {name}")
    value = bytes(
        int(item, 16)
        for item in re.findall(r"0x([0-9a-fA-F]{2})", block.group(1))
    )
    require(len(value) == size, f"embedded byte length: {name}")
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
    require(phoff + phnum * phentsize <= len(data), "program table bounds")
    require(shoff + shnum * shentsize <= len(data), "section table bounds")

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
    require(dynsym[9] == 24 and dynsym[5] % 24 == 0, "DYNSYM ABI")
    strings_section = sections[dynsym[6]]
    require(strings_section[1] == 3, "DYNSYM string link")
    strings = data[strings_section[4]:strings_section[4] + strings_section[5]]

    found = {}
    indices = {}
    for index, offset in enumerate(range(dynsym[4], dynsym[4] + dynsym[5], 24)):
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
        expected_type, minimum_size = NAMES[name]
        require(info >> 4 == 1 and info & 0xF == expected_type and
                size >= minimum_size, f"export ABI: {name}")
        found[name] = (value, size)
        indices[name] = index
    require(set(found) == set(NAMES), "missing required exports")

    relocations = {}
    for section in sections:
        if section[1] != 4:
            continue
        require(section[9] == 24 and section[5] % 24 == 0, "RELA ABI")
        for offset in range(section[4], section[4] + section[5], 24):
            target, info, addend = struct.unpack_from("<QQq", data, offset)
            require(target not in relocations,
                    f"duplicate relocation: 0x{target:x}")
            relocations[target] = (info & 0xFFFFFFFF, info >> 32, addend)

    build_ids = []
    for section in sections:
        if section[1] != 7:
            continue
        cursor = section[4]
        limit = cursor + section[5]
        while cursor < limit:
            require(cursor + 12 <= limit, "short note header")
            namesz, descsz, note_type = struct.unpack_from("<III", data, cursor)
            cursor += 12
            name = data[cursor:cursor + namesz]
            cursor += (namesz + 3) & ~3
            desc = data[cursor:cursor + descsz]
            cursor += (descsz + 3) & ~3
            require(cursor <= limit, "note bounds")
            if note_type == 3 and name == b"GNU\0":
                build_ids.append(desc)
    return data, found, indices, relocations, loads, build_ids


def vaddr_bytes(data: bytes, loads, address: int, size: int) -> bytes:
    for offset, vaddr, filesz, _, _ in loads:
        if vaddr <= address and address + size <= vaddr + filesz:
            start = offset + address - vaddr
            return data[start:start + size]
    raise AssertionError(f"virtual address not file-backed: 0x{address:x}")


def verify_source_policy(text: str) -> None:
    for needle in (
        "VerifyBuildId", "NT_GNU_BUILD_ID", "kExpectedBuildId",
        "common::HashFile", "kExpectedSha256", "ReadPayloadMappings",
        "SHT_DYNSYM", "SHT_RELA", "R_AARCH64_RELATIVE",
        "R_AARCH64_ABS64", "consumer_slot.symbol != consumer_symbol_index",
        "runtime_slots[2] != layout.persistent_consumer",
        "layout.mailbox_size != 192", "common::ReadAt(process_mem",
        "mapping.begin == expected_begin",
        "require_file_backed ? program.p_filesz : program.p_memsz",
        "PF_R | PF_X, true", "PF_R | PF_W, false",
    ):
        require(needle in text, f"resolver policy missing: {needle}")
    for needle in (
        "pwrite", "ptrace", "dlopen", "dlsym", "NativeBridge",
        "process_vm_writev", "PTRACE_", "mprotect",
    ):
        require(needle not in text, f"forbidden primitive: {needle}")


def main() -> int:
    payload = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_PAYLOAD
    if len(sys.argv) > 2:
        raise SystemExit(f"usage: {sys.argv[0]} [payload]")
    text = HEADER.read_text(encoding="utf-8")
    verify_source_policy(text)
    data, symbols, indices, relocations, loads, build_ids = parse_elf(payload)
    require(hashlib.sha256(data).digest() == c_bytes(text, "kExpectedSha256", 32),
            "payload SHA-256 pin")
    require(build_ids == [c_bytes(text, "kExpectedBuildId", 20)],
            "GNU Build ID pin")

    pointer_names = (
        "a9tas_natural_action_lifecycle_shadow_data_v1",
        "a9tas_natural_action_lifecycle_control_data_v1",
        "a9tas_natural_action_lifecycle_evidence_data_v1",
        "a9tas_natural_action_lifecycle_mailbox_data_v1",
        "a9tas_natural_action_lifecycle_object_data_v1",
        "a9tas_natural_action_lifecycle_vtable_data_v1",
    )
    targets = {}
    for name in pointer_names:
        locator = symbols[name][0]
        require(relocations.get(locator, (0, 0, 0))[:2] == (1027, 0),
                f"RELATIVE locator: {name}")
        targets[name] = relocations[locator][2]
    mailbox_rva = targets["a9tas_natural_action_lifecycle_mailbox_data_v1"]
    mailbox_file_backed = any(
        vaddr <= mailbox_rva and mailbox_rva + 192 <= vaddr + filesz
        for _, vaddr, filesz, _, flags in loads if flags & 6 == 6
    )
    mailbox_memory_backed = any(
        vaddr <= mailbox_rva and mailbox_rva + 192 <= vaddr + memsz
        for _, vaddr, _, memsz, flags in loads if flags & 6 == 6
    )
    require(mailbox_memory_backed and not mailbox_file_backed,
            "mailbox must exercise writable PT_LOAD BSS validation")
    object_rva = targets["a9tas_natural_action_lifecycle_object_data_v1"]
    vptr_rva = targets["a9tas_natural_action_lifecycle_vtable_data_v1"]
    require(relocations.get(object_rva) == (1027, 0, vptr_rva),
            "object vptr relocation")
    slot_zero = relocations.get(vptr_rva)
    require(slot_zero is not None and slot_zero[:2] == (1027, 0),
            "fail-safe slot relocation")
    require(relocations.get(vptr_rva + 8) == slot_zero,
            "matching fail-safe slots")
    require(relocations.get(vptr_rva + 16) == (
        257, indices["a9tas_natural_action_persistent_consumer_v1"], 0
    ), "persistent consumer vslot relocation")
    require(vaddr_bytes(data, loads, vptr_rva - 16, 16) == b"\0" * 16,
            "Itanium vtable prefix")

    for name, expected in (
        ("a9tas_natural_action_lifecycle_control_size_v1", 128),
        ("a9tas_natural_action_lifecycle_evidence_size_v1", 256),
        ("a9tas_natural_action_lifecycle_mailbox_size_v1", 192),
    ):
        require(struct.unpack("<Q", vaddr_bytes(
            data, loads, symbols[name][0], 8
        ))[0] == expected, f"size ABI: {name}")
    print("NATURAL_ACTION_LIFECYCLE_ELF_RESOLVER passed=1 sha256=1 "
          "build_id=1 elf64=1 aarch64=1 dynsym=11 relative_locators=6 "
          "object_vptr=1 persistent_vslot=1 guest_calls=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
