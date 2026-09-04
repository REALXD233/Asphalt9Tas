#!/usr/bin/env python3
"""Generate the compact, channel-neutral ARM64 signature catalog used on-device."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import resolve_a9_build_profile_v1 as resolver


PRACTICE_VTABLES = {
    "practice_vptr0": (0x812FAD8, 32),
    "practice_vptr588": (0x812FCB8, 28),
    "practice_vptr6c0": (0x812FE30, 20),
    "practice_vptr718": (0x812FEC0, 14),
    "practice_vptr748": (0x812FF88, 18),
}


def bytes_cpp(data: bytes) -> str:
    return "{" + ",".join(f"0x{value:02x}" for value in data) + "}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-elf", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    elf = resolver.Elf(args.reference_elf)
    groups = list(resolver.ROLE_GROUPS)
    roles = []
    for group_index, group in enumerate(groups):
        for name, rva in resolver.ROLE_GROUPS[group].items():
            size = resolver.ROLE_SIGNATURE_SIZES.get(name, resolver.SIGNATURE_SIZE)
            signature = elf.bytes_at_rva(rva, size).ljust(resolver.SIGNATURE_SIZE, b"\0")
            roles.append((name, rva, group_index, size, signature))

    vtables = dict(resolver.VTABLE_ROLES)
    vtables.update(PRACTICE_VTABLES)
    table_rows = []
    for name, (rva, minimum) in vtables.items():
        pointers, signatures, mapped = [], [], []
        for slot in range(0, resolver.VTABLE_SCAN_SIZE, 8):
            pointer = int.from_bytes(elf.bytes_at_rva(rva + slot, 8), "little")
            pointers.append(pointer)
            if elf.executable_rva(pointer, resolver.SIGNATURE_SIZE):
                signatures.append(elf.bytes_at_rva(pointer, resolver.SIGNATURE_SIZE))
                mapped.append(1)
            else:
                signatures.append(bytes(resolver.SIGNATURE_SIZE))
                mapped.append(0)
        table_rows.append((name, rva, minimum, pointers, signatures, mapped))

    adjusted = [int.from_bytes(elf.bytes_at_rva(
        resolver.REFERENCE_ADJUSTED_VTABLE_RVA + slot, 8), "little")
        for slot in range(0, resolver.VTABLE_SIZE, 8)]
    adjusted_count = len(elf.find_pointer_pair(
        resolver.ROLE_GROUPS["gameplay"]["adjusted_brake_setter"],
        resolver.ROLE_GROUPS["gameplay"]["adjusted_steering_setter"]))
    callsite = resolver.REFERENCE_FRAME_EVENT_SCHEDULER_RETURN_RVA - 4
    context = [int.from_bytes(elf.bytes_at_rva(callsite + rel, 4), "little")
               for rel in range(-16, 20, 4)]
    callers = elf.direct_bl_callers(
        resolver.ROLE_GROUPS["physics_submit"]["frame_event"])
    caller_index = callers.index(callsite)
    lifecycle_count = len(set(
        elf.find_pointer_sequence((
            resolver.ROLE_GROUPS["physics_submit"]["lifecycle_phase_gate"],
            resolver.ROLE_GROUPS["physics_submit"]["lifecycle_shared_enter"],
        ), 0x1D8) +
        elf.find_pointer_sequence((
            resolver.ROLE_GROUPS["physics_submit"]["lifecycle_phase_gate"],
            resolver.ROLE_GROUPS["physics_submit"]["lifecycle_derived_enter"],
        ), 0x1D8)))

    identity = {
        "reference_sha256": elf.sha256,
        "reference_build_id": elf.build_id,
        "roles": [(n, r, g, s, sig.hex()) for n, r, g, s, sig in roles],
        "vtables": [(n, r, m) for n, r, m, *_ in table_rows],
        "caller_index": caller_index,
        "caller_count": len(callers),
        "adjusted_count": adjusted_count,
        "lifecycle_count": lifecycle_count,
    }
    catalog_sha = hashlib.sha256(json.dumps(
        identity, sort_keys=True, separators=(",", ":")).encode()).digest()

    out = []
    out += ["#pragma once", "", "#include <array>", "#include <cstdint>", "",
            "namespace a9tas::arm64_profile_catalog_v1 {",
            "inline constexpr std::uint32_t kVersion = 1;",
            f"inline constexpr std::array<std::uint8_t,32> kCatalogSha = {bytes_cpp(catalog_sha)};",
            f"inline constexpr std::uint64_t kAdjustedVtableRva = 0x{resolver.REFERENCE_ADJUSTED_VTABLE_RVA:x}ULL;",
            f"inline constexpr std::uint32_t kAdjustedCandidateCount = {adjusted_count};",
            f"inline constexpr std::uint64_t kSchedulerReturnRva = 0x{resolver.REFERENCE_FRAME_EVENT_SCHEDULER_RETURN_RVA:x}ULL;",
            f"inline constexpr std::uint32_t kSchedulerCallerIndex = {caller_index};",
            f"inline constexpr std::uint32_t kSchedulerCallerCount = {len(callers)};",
            f"inline constexpr std::uint32_t kLifecycleVtableCount = {lifecycle_count};", ""]
    out += ["struct Role {", "  const char* name;", "  std::uint64_t reference_rva;",
            "  std::uint8_t group;", "  std::uint8_t size;",
            "  std::array<std::uint8_t,28> signature;", "};",
            f"inline constexpr std::array<Role,{len(roles)}> kRoles = {{{{"]
    for name, rva, group, size, signature in roles:
        out.append(f'  {{"{name}",0x{rva:x}ULL,{group},{size},{bytes_cpp(signature)}}},')
    out += ["}};", "", "struct Vtable {", "  const char* name;",
            "  std::uint64_t reference_rva;", "  std::uint16_t minimum;",
            "  std::array<std::uint64_t,64> pointers;",
            "  std::array<std::array<std::uint8_t,28>,64> signatures;",
            "  std::array<std::uint8_t,64> mapped;", "};",
            f"inline constexpr std::array<Vtable,{len(table_rows)}> kVtables = {{{{"]
    for name, rva, minimum, pointers, signatures, mapped in table_rows:
        pointer_text = "{" + ",".join(f"0x{x:x}ULL" for x in pointers) + "}"
        signatures_text = "{{" + ",".join(bytes_cpp(x) for x in signatures) + "}}"
        mapped_text = "{" + ",".join(str(x) for x in mapped) + "}"
        out.append(f'  {{"{name}",0x{rva:x}ULL,{minimum},{pointer_text},{signatures_text},{mapped_text}}},')
    out += ["}};", "",
            f"inline constexpr std::array<std::uint64_t,{len(adjusted)}> kAdjustedVtable = {{" +
            ",".join(f"0x{x:x}ULL" for x in adjusted) + "};",
            "inline constexpr std::array<std::uint32_t,9> kSchedulerContext = {" +
            ",".join(f"0x{x:08x}u" for x in context) + "};",
            "}  // namespace a9tas::arm64_profile_catalog_v1", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"A9_ARM64_PROFILE_CATALOG passed=1 roles={len(roles)} "
          f"vtables={len(table_rows)} lifecycle={lifecycle_count} "
          f"sha256={catalog_sha.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
