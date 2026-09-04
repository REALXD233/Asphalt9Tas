#!/usr/bin/env python3
"""Offline ELF/provenance proof for the final-writer payload resolver."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "final_writer_replay_elf_resolver_v1.h"
PROTOCOL = ROOT / "src" / "final_writer_replay_protocol_v1.h"
DEFAULT_PAYLOAD = (
    ROOT / "build" / "final-writer-replay-v1" /
    "liba9tas_final_writer_replay_v1_build_only.so"
)

NAMES = {
    "a9tas_final_writer_replay_callback_v1": (2, 24),
    "a9tas_final_writer_replay_shadow_storage_data_v1": (1, 8),
    "a9tas_final_writer_replay_control_storage_data_v1": (1, 8),
    "a9tas_final_writer_replay_target_storage_data_v1": (1, 8),
    "a9tas_final_writer_replay_audit_storage_data_v1": (1, 8),
    "a9tas_final_writer_replay_evidence_storage_data_v1": (1, 8),
    "a9tas_final_writer_replay_control_size_data_v1": (1, 8),
    "a9tas_final_writer_replay_target_size_data_v1": (1, 8),
    "a9tas_final_writer_replay_audit_size_data_v1": (1, 8),
    "a9tas_final_writer_replay_evidence_size_data_v1": (1, 8),
}

POINTER_NAMES = tuple(name for name in NAMES if "storage_data" in name)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def embedded_hash(text: str) -> bytes:
    block = re.search(r"kExpectedSha256\[32\].*?\{(.*?)\};", text, re.DOTALL)
    require(block is not None, "embedded hash missing")
    value = bytes(int(item, 16) for item in
                  re.findall(r"0x([0-9a-fA-F]{2})", block.group(1)))
    require(len(value) == 32, "embedded hash size")
    return value


def parse(path: pathlib.Path):
    data = path.read_bytes()
    require(len(data) >= 64, "short ELF")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    ident, elf_type, machine = header[0], header[1], header[2]
    phoff, shoff = header[5], header[6]
    phentsize, phnum, shentsize, shnum = header[9:13]
    require(ident[:4] == b"\x7fELF" and ident[4:6] == b"\x02\x01",
            "not little-endian ELF64")
    require(elf_type == 3 and machine == 183, "not AArch64 ET_DYN")
    require(phentsize == 56 and shentsize == 64 and 0 < shnum <= 256,
            "ELF table ABI")
    require(phoff + phnum * phentsize <= len(data), "program table bounds")
    require(shoff + shnum * shentsize <= len(data), "section table bounds")

    loads = []
    for index in range(phnum):
        item = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        if item[0] == 1:
            loads.append((item[2], item[3], item[5], item[6], item[1]))
    sections = [
        struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    dynsyms = [section for section in sections if section[1] == 11]
    require(len(dynsyms) == 1, "one DYNSYM required")
    dynsym = dynsyms[0]
    require(dynsym[9] == 24 and dynsym[5] % 24 == 0, "DYNSYM ABI")
    strings_section = sections[dynsym[6]]
    require(strings_section[1] == 3, "DYNSYM string link")
    strings = data[strings_section[4]:strings_section[4] + strings_section[5]]

    found = {}
    for offset in range(dynsym[4], dynsym[4] + dynsym[5], 24):
        name_offset, info, _, section_index, value, size = struct.unpack_from(
            "<IBBHQQ", data, offset)
        if name_offset >= len(strings) or section_index == 0:
            continue
        end = strings.find(b"\0", name_offset)
        require(end >= 0, "unterminated symbol")
        name = strings[name_offset:end].decode("ascii")
        if name not in NAMES:
            continue
        require(name not in found and info >> 4 == 1, f"unique global {name}")
        symbol_type, minimum_size = NAMES[name]
        require(info & 0xF == symbol_type and size >= minimum_size,
                f"symbol type/size {name}")
        found[name] = (value, size)
    require(set(found) == set(NAMES), "missing exports")

    relocations = {}
    for section in sections:
        if section[1] != 4:
            continue
        require(section[9] == 24 and section[5] % 24 == 0, "RELA ABI")
        for offset in range(section[4], section[4] + section[5], 24):
            target, info, addend = struct.unpack_from("<QQq", data, offset)
            relocations[target] = (info & 0xFFFFFFFF, addend)
    for name in POINTER_NAMES:
        locator = found[name][0]
        require(locator in relocations and relocations[locator][0] == 1027,
                f"AArch64 RELATIVE locator {name}")
        target = relocations[locator][1]
        require(any((flags & 0x6) == 0x6 and vaddr <= target < vaddr + memsz
                    for _, vaddr, _, memsz, flags in loads),
                f"locator target outside writable PT_LOAD {name}")
    return data, found, loads


def verify_policy(text: str) -> None:
    required = (
        "ReadMappings", "ResolveSymbols", "HashFile", "kExpectedSha256",
        "EM_AARCH64", "SHT_DYNSYM", "mapping.begin == expected_begin",
        "locator_addresses", "WritableRange", "sizeof(FrameTarget) * kMaximumFrames",
        "layout.control_size != sizeof(Control)",
        "layout.target_size != sizeof(FrameTarget)",
        "layout.audit_size != sizeof(FrameAudit)",
        "layout.evidence_size != sizeof(Evidence)",
    )
    for needle in required:
        require(needle in text, f"resolver policy missing {needle}")
    for needle in ("pwrite", "process_vm_writev", "mprotect(", "dlopen(",
                   "dlsym(", "NativeBridge"):
        require(needle not in text, f"forbidden resolver primitive {needle}")
    protocol = PROTOCOL.read_text(encoding="utf-8")
    for needle in ("sizeof(FrameTarget) == 80", "sizeof(FrameAudit) == 160",
                   "sizeof(Control) == 128", "sizeof(Evidence) == 192"):
        require(needle in protocol, f"protocol ABI missing {needle}")


def main() -> int:
    payload = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_PAYLOAD
    require(len(sys.argv) <= 2, f"usage: {sys.argv[0]} [payload]")
    text = HEADER.read_text(encoding="utf-8")
    verify_policy(text)
    data, found, loads = parse(payload)
    require(hashlib.sha256(data).digest() == embedded_hash(text),
            "payload hash differs from resolver pin")
    wrapper = found["a9tas_final_writer_replay_callback_v1"][0]
    require(any((flags & 0x5) == 0x5 and vaddr <= wrapper and
                wrapper + 24 <= vaddr + filesz
                for _, vaddr, filesz, _, flags in loads),
            "wrapper outside executable file-backed PT_LOAD")
    print("FINAL_WRITER_ELF_RESOLVER passed=1 sha256=1 aarch64=1 dynsym=10 "
          "relative_locators=5 max_frames=3600 guest_calls=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
