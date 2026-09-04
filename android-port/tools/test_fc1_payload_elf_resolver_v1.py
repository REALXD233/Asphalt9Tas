#!/usr/bin/env python3
"""Offline proof for the no-guest-call FC-1 ARM64 ELF resolver."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc1_payload_elf_resolver_v1.h"
CONTROLLER = ROOT / "src" / "fc1_frame_callback_transaction_controller_v1.cpp"
DEFAULT_PAYLOAD = (
    ROOT / "build" / "frame-callback-bootstrap-v1" /
    "liba9tas_frame_callback_bootstrap_v1_build_only.so"
)

NAMES = {
    "a9tas_fc0_frame_callback_passthrough_v1": (2, 24),
    "a9tas_fc0_frame_callback_shadow_storage_data_v1": (1, 8),
    "a9tas_fc0_frame_callback_control_storage_data_v1": (1, 8),
    "a9tas_fc0_frame_callback_evidence_storage_data_v1": (1, 8),
    "a9tas_fc0_frame_callback_control_size_data_v1": (1, 8),
    "a9tas_fc0_frame_callback_evidence_size_data_v1": (1, 8),
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


def parse_elf(path: pathlib.Path) -> tuple[bytes, dict[str, tuple[int, int, int]], list[tuple[int, int, int, int, int]]]:
    data = path.read_bytes()
    require(len(data) >= 64, "short ELF")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    ident, elf_type, machine = header[0], header[1], header[2]
    phoff, shoff = header[5], header[6]
    phentsize, phnum, shentsize, shnum = header[9], header[10], header[11], header[12]
    require(ident[:4] == b"\x7fELF" and ident[4] == 2 and ident[5] == 1,
            "not little-endian ELF64")
    require(elf_type == 3 and machine == 183, "not AArch64 ET_DYN")
    require(phentsize == 56 and shentsize == 64 and 0 < shnum <= 256,
            "ELF table ABI")
    require(phoff + phnum * phentsize <= len(data), "program table out of bounds")
    require(shoff + shnum * shentsize <= len(data), "section table out of bounds")

    loads: list[tuple[int, int, int, int, int]] = []
    for index in range(phnum):
        item = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        p_type, flags, offset, vaddr, _, filesz, memsz, _ = item
        if p_type == 1:
            loads.append((offset, vaddr, filesz, memsz, flags))

    sections = [
        struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    dynsyms = [section for section in sections if section[1] == 11]
    require(len(dynsyms) == 1, "required one DYNSYM")
    dynsym = dynsyms[0]
    require(dynsym[9] == 24 and dynsym[5] % 24 == 0, "DYNSYM entry ABI")
    require(dynsym[6] < len(sections), "DYNSYM string link")
    strings_section = sections[dynsym[6]]
    require(strings_section[1] == 3, "DYNSYM link is not STRTAB")
    strings = data[strings_section[4]: strings_section[4] + strings_section[5]]
    require(len(strings) == strings_section[5], "short dynamic string table")

    found: dict[str, tuple[int, int, int]] = {}
    for offset in range(dynsym[4], dynsym[4] + dynsym[5], 24):
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
    require(set(found) == set(NAMES), "missing required dynamic exports")

    # Verify three pointer locator objects have RELATIVE relocations.  The
    # shadow addend must be fully covered by file bytes, not anonymous BSS.
    relocations: dict[int, tuple[int, int]] = {}
    for section in sections:
        if section[1] != 4:
            continue
        require(section[9] == 24 and section[5] % 24 == 0, "RELA entry ABI")
        for offset in range(section[4], section[4] + section[5], 24):
            target, info, addend = struct.unpack_from("<QQq", data, offset)
            relocations[target] = (info & 0xFFFFFFFF, addend)
    pointer_names = (
        "a9tas_fc0_frame_callback_shadow_storage_data_v1",
        "a9tas_fc0_frame_callback_control_storage_data_v1",
        "a9tas_fc0_frame_callback_evidence_storage_data_v1",
    )
    for name in pointer_names:
        symbol_value = found[name][0]
        require(symbol_value in relocations and relocations[symbol_value][0] == 1027,
                f"missing AArch64 RELATIVE relocation: {name}")
    shadow_addend = relocations[found[pointer_names[0]][0]][1]
    require(any(vaddr <= shadow_addend and shadow_addend + 0x908 <= vaddr + filesz
                for _, vaddr, filesz, _, _ in loads),
            "shadow is not fully file-backed")
    return data, found, loads


def vaddr_bytes(data: bytes, loads: list[tuple[int, int, int, int, int]],
                address: int, size: int) -> bytes:
    for offset, vaddr, filesz, _, _ in loads:
        if vaddr <= address and address + size <= vaddr + filesz:
            start = offset + address - vaddr
            return data[start:start + size]
    raise AssertionError("virtual address not file-backed")


def verify_source_policy(header_text: str) -> None:
    required = (
        "ReadPayloadMappings", "ResolveSymbols", "SHT_DYNSYM",
        "EM_AARCH64", "HashFile", "kExpectedSha256",
        "layout.control_size != 64", "layout.evidence_size != 128",
        "mapping.file_offset == 0", "mapping.end - bias > 8 * 1024 * 1024",
        "program.p_type != PT_LOAD", "mapping.begin == expected_begin",
        "locator_addresses", "mapping->path != path",
        "Houdini maps ARM64 guest .text read-only",
        "in_file_load(output->wrapper, 24, PF_R | PF_X)",
        "wrapper_map->perms[1] == 'w'",
    )
    for needle in required:
        require(needle in header_text, f"resolver policy missing: {needle}")
    for needle in ("pwrite", "ptrace", "dlopen", "dlsym", "NativeBridge",
                   "process_vm_writev"):
        require(needle not in header_text, f"resolver forbidden primitive: {needle}")

    controller = CONTROLLER.read_text(encoding="utf-8")
    require('#include "fc1_payload_elf_resolver_v1.h"' in controller,
            "controller does not use resolver")
    require("a9tas::fc1_payload_elf_v1::Resolve(pid, mem, &payload)" in controller,
            "controller resolver call missing")
    require("SHADOW_HEX" not in controller and "CONTROL_HEX" not in controller and
            "EVIDENCE_HEX" not in controller and "WRAPPER_HEX" not in controller,
            "controller still accepts externally supplied payload addresses")


def main() -> int:
    payload = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_PAYLOAD
    if len(sys.argv) > 2:
        raise SystemExit(f"usage: {sys.argv[0]} [payload]")
    header_text = HEADER.read_text(encoding="utf-8")
    verify_source_policy(header_text)
    data, symbols, loads = parse_elf(payload)
    require(hashlib.sha256(data).digest() == c_expected_hash(header_text),
            "payload SHA-256 differs from resolver pin")
    wrapper = symbols["a9tas_fc0_frame_callback_passthrough_v1"][0]
    require(any((flags & 0x5) == 0x5 and vaddr <= wrapper and
                wrapper + 24 <= vaddr + filesz
                for _, vaddr, filesz, _, flags in loads),
            "wrapper is not covered by an ELF PF_R|PF_X file-backed PT_LOAD")
    signature = vaddr_bytes(data, loads, wrapper, 24)
    controller = CONTROLLER.read_text(encoding="utf-8")
    block = re.search(r"kWrapperSignature\[24\]\s*=\s*\{(.*?)\};",
                      controller, re.DOTALL)
    require(block is not None, "controller wrapper signature missing")
    expected_signature = bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9a-fA-F]{2})", block.group(1))
    )
    require(signature == expected_signature, "wrapper signature mismatch")
    print("FC1_ELF_RESOLVER passed=1 sha256=1 elf64=1 aarch64=1 dynsym=6 "
          "relative_locators=3 shadow_file_backed=1 guest_calls=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
