#!/usr/bin/env python3
"""Offline file-layout proof for the fixed-RVA phase-observer resolver."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "vehicle_camera_phase_observer_elf_resolver_v1.h"
DEFAULT_PAYLOAD = (ROOT / "build" / "vehicle-camera-phase-observer-v1" /
                   "liba9tas_vehicle_camera_phase_observer_v1_build_only.so")

EXPECTED = {
    "a9tas_vehicle_camera_phase_vehicle_callback_v1": 0x2DB8,
    "a9tas_vehicle_camera_phase_camera_callback_v1": 0x32B4,
    "a9tas_final_writer_replay_shadow_storage_data_v1": 0x6600,
    "a9tas_final_writer_replay_control_storage_data_v1": 0x6608,
    "a9tas_final_writer_replay_target_storage_data_v1": 0x6610,
    "a9tas_final_writer_replay_audit_storage_data_v1": 0x6618,
    "a9tas_final_writer_replay_evidence_storage_data_v1": 0x6620,
    "a9tas_final_writer_replay_control_size_data_v1": 0x6628,
    "a9tas_final_writer_replay_target_size_data_v1": 0x6630,
    "a9tas_final_writer_replay_audit_size_data_v1": 0x6638,
    "a9tas_final_writer_replay_evidence_size_data_v1": 0x6640,
    "a9tas_vehicle_camera_phase_control_storage_v1": 0x7040,
    "a9tas_vehicle_camera_phase_evidence_storage_v1": 0x7140,
    "a9tas_vehicle_camera_phase_events_storage_v1": 0x7148,
}

EXPECTED_RELOCATION_TARGETS = {
    0x6600: 0x6680,
    0x6608: 0x64C0,
    0x6610: 0x93B80,
    0x6618: 0x7180,
    0x6620: 0x6540,
    0x7040: 0x6FC0,
    0x7140: 0x7080,
    0x7148: 0xDA0C0,
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def embedded_hash(text: str) -> bytes:
    block = re.search(r"kExpectedSha256\[32\].*?\{(.*?)\};", text, re.DOTALL)
    require(block is not None, "resolver hash missing")
    value = bytes(int(item, 16) for item in
                  re.findall(r"0x([0-9a-fA-F]{2})", block.group(1)))
    require(len(value) == 32, "resolver hash length")
    return value


def parse_elf(data: bytes):
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    require(header[0][:6] == b"\x7fELF\x02\x01" and header[1] == 3 and
            header[2] == 183, "payload is not AArch64 ET_DYN")
    shoff, shentsize, shnum = header[6], header[11], header[12]
    sections = [struct.unpack_from("<IIQQQQIIQQ", data,
                                   shoff + index * shentsize)
                for index in range(shnum)]
    dynsym = [item for item in sections if item[1] == 11]
    require(len(dynsym) == 1, "exactly one dynamic symbol table required")
    sym = dynsym[0]
    strings_header = sections[sym[6]]
    strings = data[strings_header[4]:strings_header[4] + strings_header[5]]
    found = {}
    for offset in range(sym[4], sym[4] + sym[5], sym[9]):
        name_offset, info, _, section, value, size = struct.unpack_from(
            "<IBBHQQ", data, offset)
        if section == 0 or name_offset >= len(strings):
            continue
        end = strings.find(b"\0", name_offset)
        require(end >= 0, "unterminated symbol name")
        name = strings[name_offset:end].decode("ascii")
        if name in EXPECTED:
            require(name not in found and info >> 4 == 1 and size >= 8,
                    f"bad exported symbol {name}")
            found[name] = value
    require(found == EXPECTED, "exported RVA set differs from resolver constants")

    relocations = {}
    for section in sections:
        if section[1] != 4:
            continue
        for offset in range(section[4], section[4] + section[5], section[9]):
            target, info, addend = struct.unpack_from("<QQq", data, offset)
            relocations[target] = (info & 0xFFFFFFFF, addend)
    for locator, target in EXPECTED_RELOCATION_TARGETS.items():
        require(relocations.get(locator) == (1027, target),
                f"locator relocation mismatch at {locator:#x}")


def verify_header(text: str) -> None:
    for needle in (
        "HashFile", "kExpectedSha256", "ReadMappings", "ReadPointer",
        "kVehicleWrapperRva = 0x2DB8", "kCameraWrapperRva = 0x32B4",
        "kExpectedPhaseEventsStorageRva = 0xDA0C0", "WritableRange",
        "sizeof(phase::Event) * phase::kMaximumEvents",
    ):
        require(needle in text, f"resolver source contract missing {needle}")
    require("vehicle_map->perms[0] != 'r'" in text and
            "vehicle_map->perms[1] == 'w'" in text and
            "camera_map->perms[0] != 'r'" in text and
            "camera_map->perms[1] == 'w'" in text,
            "Houdini-compatible read-only wrapper mapping proof missing")
    require("perms[2] != 'x'" not in text,
            "resolver incorrectly requires guest ELF execute permission")
    for needle in ("pwrite", "process_vm_writev", "mprotect(", "dlopen(",
                   "dlsym(", "ptrace("):
        require(needle not in text, f"forbidden resolver primitive {needle}")


def main() -> int:
    payload = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_PAYLOAD
    require(len(sys.argv) <= 2, f"usage: {sys.argv[0]} [payload]")
    text = HEADER.read_text(encoding="utf-8")
    data = payload.read_bytes()
    verify_header(text)
    require(hashlib.sha256(data).digest() == embedded_hash(text),
            "payload differs from resolver hash pin")
    parse_elf(data)
    print("VEHICLE_CAMERA_PHASE_ELF_RESOLVER passed=1 sha256=1 "
          "fixed_rva=1 relative_locators=8 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
