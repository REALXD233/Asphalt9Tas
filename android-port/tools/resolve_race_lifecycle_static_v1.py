#!/usr/bin/env python3
"""Resolve the pinned Android race-lifecycle state machine from libAsphalt9.

This is an offline-only resolver.  It reads one ELF file, validates the exact
CN 6.0.0k build hash, and derives the race-phase entry points plus every race
vtable that exposes the shared phase gate.  It never opens a process or a
device.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
from dataclasses import dataclass


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_LIBRARY = ROOT / "ida" / "a9cn_600k" / "libAsphalt9.so"

EXPECTED_SHA256 = (
    "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0"
)

ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")

PT_LOAD = 1
ET_DYN = 3
EM_AARCH64 = 183
PF_X = 1

STATE_FIELD_OFFSET = 0x2D8
PHASE_GATE_SLOT = 0x1D8
PHASE_ENTER_SLOT = 0x1E0
INTRO_STATE = 1
COUNTDOWN_STATE = 2
RACING_STATE = 3

# Exact instruction sequences from the hash-pinned binary.  Each is required
# to occur once in executable file-backed memory.
PHASE_GATE_PATTERN = bytes.fromhex(
    "08 d8 42 b9 1f 09 00 71 81 00 00 54 08 00 40 f9 "
    "01 f1 40 f9 20 00 1f d6 c0 03 5f d6"
)
INTRO_TRANSITION_PATTERN = bytes.fromhex(
    "08 d8 42 b9 68 00 00 34 f3 7b c1 a8 c0 03 5f d6"
)
COUNTDOWN_TRANSITION_PATTERN = bytes.fromhex(
    "08 d8 42 b9 1f 05 00 71 c1 01 00 54 21 00 80 52 "
    "f3 03 00 aa"
)
RACING_TRANSITION_PATTERN = bytes.fromhex(
    "60 7e 40 f9 61 00 80 52 61 da 02 b9 20 02 00 b4"
)
RACING_ENTRY_PATTERN = bytes.fromhex(
    "ff 83 03 d1 f7 5b 0b a9 f5 53 0c a9 f3 7b 0d a9 "
    "71 2b 00 94 e8 57 00 f9 08 d8 42 b9 08 01 1f 32"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


@dataclass(frozen=True)
class LoadSegment:
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int

    def contains_file_va(self, address: int, size: int = 1) -> bool:
        return self.vaddr <= address and address + size <= self.vaddr + self.filesz

    def executable(self) -> bool:
        return bool(self.flags & PF_X)


def parse_loads(data: bytes) -> list[LoadSegment]:
    require(len(data) >= ELF_HEADER.size, "short ELF")
    header = ELF_HEADER.unpack_from(data)
    ident, elf_type, machine = header[0], header[1], header[2]
    phoff, phentsize, phnum = header[5], header[9], header[10]
    require(ident[:4] == b"\x7fELF" and ident[4:6] == b"\x02\x01",
            "not little-endian ELF64")
    require(elf_type == ET_DYN and machine == EM_AARCH64,
            "not AArch64 ET_DYN")
    require(phentsize == PROGRAM_HEADER.size and 0 < phnum <= 256,
            "program-header ABI")
    require(phoff + phentsize * phnum <= len(data),
            "program-header bounds")
    loads: list[LoadSegment] = []
    for index in range(phnum):
        item = PROGRAM_HEADER.unpack_from(data, phoff + index * phentsize)
        if item[0] != PT_LOAD:
            continue
        segment = LoadSegment(item[2], item[3], item[5], item[6], item[1])
        require(segment.offset + segment.filesz <= len(data),
                "PT_LOAD file bounds")
        loads.append(segment)
    require(loads and any(item.executable() for item in loads),
            "missing executable PT_LOAD")
    return loads


def file_offset_to_va(loads: list[LoadSegment], offset: int) -> int:
    matches = [item for item in loads
               if item.offset <= offset < item.offset + item.filesz]
    require(len(matches) == 1, "file offset does not map to one PT_LOAD")
    item = matches[0]
    return item.vaddr + offset - item.offset


def va_to_file_offset(loads: list[LoadSegment], address: int,
                      size: int = 1) -> int:
    matches = [item for item in loads if item.contains_file_va(address, size)]
    require(len(matches) == 1, "VA does not map to one file-backed PT_LOAD")
    item = matches[0]
    return item.offset + address - item.vaddr


def executable_va(loads: list[LoadSegment], address: int) -> bool:
    return any(item.executable() and item.contains_file_va(address)
               for item in loads)


def find_pattern_vas(data: bytes, loads: list[LoadSegment], pattern: bytes,
                     executable: bool | None = None) -> list[int]:
    require(pattern, "empty pattern")
    result: list[int] = []
    for item in loads:
        if executable is not None and item.executable() != executable:
            continue
        blob = data[item.offset:item.offset + item.filesz]
        cursor = 0
        while True:
            found = blob.find(pattern, cursor)
            if found < 0:
                break
            result.append(item.vaddr + found)
            cursor = found + 1
    return result


def find_unique_executable(data: bytes, loads: list[LoadSegment],
                           pattern: bytes, label: str) -> int:
    matches = find_pattern_vas(data, loads, pattern, executable=True)
    require(len(matches) == 1,
            f"{label} signature count {len(matches)} instead of 1")
    return matches[0]


def read_u64(data: bytes, loads: list[LoadSegment], address: int) -> int:
    offset = va_to_file_offset(loads, address, 8)
    return struct.unpack_from("<Q", data, offset)[0]


def resolve(path: pathlib.Path) -> dict[str, object]:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    require(digest == EXPECTED_SHA256, "libAsphalt9 SHA-256 mismatch")
    loads = parse_loads(data)

    phase_gate = find_unique_executable(
        data, loads, PHASE_GATE_PATTERN, "phase gate")
    intro_check = find_unique_executable(
        data, loads, INTRO_TRANSITION_PATTERN, "intro transition")
    countdown_check = find_unique_executable(
        data, loads, COUNTDOWN_TRANSITION_PATTERN, "countdown transition")
    racing_context = find_unique_executable(
        data, loads, RACING_TRANSITION_PATTERN, "racing transition")
    racing_entry = find_unique_executable(
        data, loads, RACING_ENTRY_PATTERN, "racing phase entry")

    # The exact state-store instruction is the third instruction in the
    # four-instruction racing context.
    racing_store = racing_context + 8

    gate_pointer = struct.pack("<Q", phase_gate)
    gate_refs = find_pattern_vas(data, loads, gate_pointer, executable=False)
    require(len(gate_refs) == 294, "unexpected race-vtable family size")

    vtables: list[dict[str, object]] = []
    implementation_counts: dict[int, int] = {}
    for gate_ref in gate_refs:
        vptr = gate_ref - PHASE_GATE_SLOT
        require(read_u64(data, loads, vptr + PHASE_GATE_SLOT) == phase_gate,
                "phase-gate slot mismatch")
        enter = read_u64(data, loads, vptr + PHASE_ENTER_SLOT)
        require(executable_va(loads, enter),
                "phase-enter slot is not executable")
        implementation_counts[enter] = implementation_counts.get(enter, 0) + 1
        vtables.append({
            "vptr_rva": f"0x{vptr:x}",
            "phase_enter_rva": f"0x{enter:x}",
        })

    require(implementation_counts.get(racing_entry, 0) == 284,
            "shared phase-enter implementation count")
    require(len(implementation_counts) == 2 and
            sorted(implementation_counts.values()) == [10, 284],
            "unexpected phase-enter override set")

    implementations = [
        {"rva": f"0x{address:x}", "vtable_count": count}
        for address, count in sorted(implementation_counts.items())
    ]
    return {
        "schema": "A9RLS1",
        "binary": str(path.resolve()),
        "binary_sha256": digest,
        "device_access": 0,
        "gameplay_writes": 0,
        "phase_state": {
            "field_offset": f"0x{STATE_FIELD_OFFSET:x}",
            "intro": INTRO_STATE,
            "countdown": COUNTDOWN_STATE,
            "racing": RACING_STATE,
        },
        "rvas": {
            "phase_gate": f"0x{phase_gate:x}",
            "intro_state_check": f"0x{intro_check:x}",
            "countdown_state_check": f"0x{countdown_check:x}",
            "racing_transition_context": f"0x{racing_context:x}",
            "racing_phase_entry": f"0x{racing_entry:x}",
            "racing_state_store": f"0x{racing_store:x}",
        },
        "vtable": {
            "phase_gate_slot": f"0x{PHASE_GATE_SLOT:x}",
            "phase_enter_slot": f"0x{PHASE_ENTER_SLOT:x}",
            "candidate_count": len(vtables),
            "implementations": implementations,
            "candidates": vtables,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", nargs="?", type=pathlib.Path,
                        default=DEFAULT_LIBRARY)
    parser.add_argument("--json", action="store_true",
                        help="emit the complete machine-readable manifest")
    args = parser.parse_args()
    result = resolve(args.library)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        implementations = result["vtable"]["implementations"]
        counts = ",".join(str(item["vtable_count"])
                          for item in implementations)
        print(
            "RACE_LIFECYCLE_STATIC passed=1 schema=A9RLS1 sha256=1 "
            f"phase_gate={result['rvas']['phase_gate']} "
            f"racing_store={result['rvas']['racing_state_store']} "
            f"vtable_candidates={result['vtable']['candidate_count']} "
            f"implementation_counts={counts} device_access=0 gameplay_writes=0"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
