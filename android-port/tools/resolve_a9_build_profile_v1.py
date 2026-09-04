#!/usr/bin/env python3
"""Resolve an A9 native-build profile without using channel/package names.

This is a read-only, offline first-stage resolver.  It uses a proven reference
ELF as a semantic anchor, locates exact function bodies in another ELF, groups
related functions by relocation delta, and derives the adjusted-input vtable
candidate structurally.  Its output is never a write authorization: live object
identity and the remaining object/vtable relationships must still be proven.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import pathlib
import struct
import sys
from dataclasses import dataclass


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
PT_LOAD = 1
PT_NOTE = 4
PF_X = 1
PF_W = 2
PF_R = 4
EM_AARCH64 = 183
SIGNATURE_SIZE = 28
VTABLE_SIZE = 0x908
SETTER_SLOT = 0x2A8


ROLE_GROUPS: dict[str, dict[str, int]] = {
    "gameplay": {
        "physics_interval": 0x3695474,
        "final_writer": 0x367D66C,
        "nitro_state": 0x36D8524,
        "barrel_roll_tail": 0x369DE4C,
        "barrel_yaw_tail": 0x369E30C,
        "adjusted_brake_setter": 0x36934B8,
        "adjusted_steering_setter": 0x36934E0,
        "nitro_dispatch": 0x3674E50,
        "vehicle_source_update": 0x36AE400,
        "car_physics_body_update": 0x36999E0,
        "vehicle_wrapper_slot40": 0x36A9940,
        "vehicle_wrapper_slot48": 0x36A9954,
        "vehicle_wrapper_slot58": 0x36A8DC8,
        "vehicle_wrapper_slot60": 0x36A8A3C,
        "vehicle_wrapper_slot68": 0x36A8A50,
        "vehicle_wrapper_slot88": 0x36A8A78,
        "vehicle_wrapper_slot90": 0x36A9C20,
        "vehicle_wrapper_slot98": 0x36A8C14,
        "vehicle_wrapper_slotA0": 0x36A8C28,
        "vehicle_delegate_slot40": 0x36AE8C0,
        "vehicle_delegate_slot48": 0x36AE8EC,
        "vehicle_delegate_slot58": 0x36ACCD4,
        "vehicle_delegate_slot60": 0x36AC558,
        "vehicle_delegate_slot68": 0x36AC570,
        "vehicle_delegate_slot88": 0x36AC590,
        "vehicle_delegate_slot90": 0x36AEA20,
        "vehicle_delegate_slot98": 0x36AC980,
        "vehicle_delegate_slotA0": 0x36AC9E0,
    },
    "physics_submit": {
        "frame_event": 0x38B78E4,
        "physics_submit": 0x38B7AC4,
        "logic_dispatcher": 0x37949C8,
        "barrel_random_bool": 0x38B7A00,
        "barrel_random_lerp": 0x38B7A3C,
        "lifecycle_phase_gate": 0x3A5955C,
        "lifecycle_shared_enter": 0x3A59578,
        "lifecycle_derived_enter": 0x382EB04,
        "lifecycle_racing_store": 0x3A596C4,
    },
    "physics_api": {
        "vehicle_position_getter": 0x4D10D38,
        "vehicle_rotation_getter": 0x4D10D4C,
        "physics_set_pose": 0x4CC58CC,
        "physics_set_position": 0x4CC5A18,
        "physics_set_rotation": 0x4CC5AAC,
        "physics_set_linear": 0x4CC5C18,
        "physics_set_angular": 0x4CC5C34,
        "physics_get_linear": 0x4CC5C50,
        "physics_get_angular": 0x4CC5C64,
    },
}

ROLE_SIGNATURE_SIZES = {
    "lifecycle_racing_store": 12,
    "vehicle_delegate_slot90": 12,
    "barrel_random_lerp": 24,
}

# Instruction masks for signatures that contain link/load-time address
# immediates.  Keep opcodes and registers exact while ignoring only the
# relocation-sensitive immediate fields.
ROLE_WORD_MASKS: dict[str, tuple[int, ...]] = {
    "logic_dispatcher": (
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0x9F00001F,  # ADRP: opcode + destination register
        0xFFC003FF,  # ADD (immediate): opcode/shift + source/destination regs
        0xFFFFFFFF,
    ),
    "barrel_random_bool": (
        0xFFFFFFFF, 0xFFFFFFFF, 0xFC000000, 0xFC000000,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    ),
    "barrel_random_lerp": (
        0xFFFFFFFF, 0xFFFFFFFF, 0xFC000000, 0xFC000000,
        0xFFFFFFFF, 0xFFFFFFFF,
    ),
}
MAX_ROLE_LOCAL_DRIFT = 0x1000

REFERENCE_ADJUSTED_VTABLE_RVA = 0x7EED9E0
REFERENCE_FRAME_EVENT_SCHEDULER_RETURN_RVA = 0x3794C78

VTABLE_ROLES: dict[str, tuple[int, int]] = {
    # role: (reference address-point RVA, minimum mapped slots)
    "main_time_source": (0x7F3C1A8, 3),
    "embedded_time_source": (0x7F3C400, 10),
    "physics_context": (0x8103830, 10),
    "physics_implementation": (0x7EECD88, 20),
    "nitro_service": (0x7EE8A90, 20),
    "vehicle_source": (0x7EF2F48, 20),
    "native_physics_body": (0x9EDC6A0, 2),
    "physics_backend": (0x9D5ED40, 20),
    "step_options": (0x7EED420, 3),
    # CarPhysicsState can expose one of several interface address-points.  The
    # live resolver scans for these exact identities before following the
    # adjustment tables, so every supported build profile must carry their
    # structurally resolved counterparts rather than reusing reference RVAs.
    "physics_interface_0": (0x7EE9B78, 6),
    "physics_interface_1": (0x80C8098, 6),
    "physics_interface_2": (0x80C9978, 6),
    "physics_interface_3": (0x80CAF40, 6),
    "physics_interface_4": (0x80E74A0, 6),
    "physics_interface_5": (0x80E8DF0, 6),
    "physics_interface_6": (0x80EA3B8, 6),
    "physics_interface_7": (0x815DC08, 6),
    "physics_interface_8": (0x815F3E8, 6),
}
VTABLE_SCAN_SIZE = 0x200


@dataclass(frozen=True)
class Load:
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int


class Elf:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < ELF_HEADER.size:
            raise ValueError("ELF is truncated")
        header = ELF_HEADER.unpack_from(self.data)
        ident, machine = header[0], header[2]
        if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
            raise ValueError("expected little-endian ELF64")
        if machine != EM_AARCH64:
            raise ValueError("expected AArch64 ELF")
        phoff, phentsize, phnum = header[5], header[9], header[10]
        if phentsize != PROGRAM_HEADER.size:
            raise ValueError("unexpected program-header size")
        self.loads: list[Load] = []
        self.build_id = ""
        for index in range(phnum):
            position = phoff + index * phentsize
            if position + phentsize > len(self.data):
                raise ValueError("program headers are truncated")
            kind, flags, offset, vaddr, _paddr, filesz, memsz, _align = (
                PROGRAM_HEADER.unpack_from(self.data, position)
            )
            if kind == PT_LOAD:
                if offset + filesz > len(self.data):
                    raise ValueError("PT_LOAD exceeds file")
                self.loads.append(Load(offset, vaddr, filesz, memsz, flags))
            elif kind == PT_NOTE:
                self._read_notes(offset, filesz)
        if not self.loads:
            raise ValueError("ELF has no PT_LOAD")

    def _read_notes(self, offset: int, size: int) -> None:
        if offset + size > len(self.data):
            raise ValueError("PT_NOTE exceeds file")
        cursor, end = offset, offset + size
        while cursor + 12 <= end:
            name_size, desc_size, kind = struct.unpack_from(
                "<III", self.data, cursor
            )
            cursor += 12
            name_end = cursor + name_size
            desc_start = (name_end + 3) & ~3
            desc_end = desc_start + desc_size
            next_note = (desc_end + 3) & ~3
            if next_note > end:
                raise ValueError("ELF note is truncated")
            name = self.data[cursor:name_end].rstrip(b"\0")
            description = self.data[desc_start:desc_end]
            if name == b"GNU" and kind == 3:
                value = description.hex()
                if self.build_id and self.build_id != value:
                    raise ValueError("multiple conflicting GNU build IDs")
                self.build_id = value
            cursor = next_note

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()

    @property
    def executable_end(self) -> int:
        return max(load.vaddr + load.memsz for load in self.loads
                   if load.flags & PF_X)

    def rva_to_offset(self, rva: int, size: int = 1) -> int:
        for load in self.loads:
            if load.vaddr <= rva and rva + size <= load.vaddr + load.filesz:
                return load.offset + rva - load.vaddr
        raise ValueError(f"RVA is not file-backed: 0x{rva:x}")

    def offset_to_rva(self, offset: int, size: int = 1) -> int:
        for load in self.loads:
            if load.offset <= offset and offset + size <= load.offset + load.filesz:
                return load.vaddr + offset - load.offset
        raise ValueError(f"file offset is not in PT_LOAD: 0x{offset:x}")

    def bytes_at_rva(self, rva: int, size: int) -> bytes:
        offset = self.rva_to_offset(rva, size)
        return self.data[offset:offset + size]

    def find_in_executable(self, needle: bytes) -> list[int]:
        hits: list[int] = []
        for load in self.loads:
            if not (load.flags & PF_X):
                continue
            region = self.data[load.offset:load.offset + load.filesz]
            cursor = 0
            while True:
                found = region.find(needle, cursor)
                if found < 0:
                    break
                hits.append(load.vaddr + found)
                cursor = found + 1
        return sorted(set(hits))

    def find_masked_in_executable(self, needle: bytes,
                                  word_masks: tuple[int, ...]) -> list[int]:
        if len(needle) != len(word_masks) * 4:
            raise ValueError("masked signature length is invalid")
        expected = [struct.unpack_from("<I", needle, index * 4)[0] & mask
                    for index, mask in enumerate(word_masks)]
        prefix = needle[:8]
        hits: list[int] = []
        for load in self.loads:
            if not (load.flags & PF_X):
                continue
            region = self.data[load.offset:load.offset + load.filesz]
            cursor = 0
            while True:
                found = region.find(prefix, cursor)
                if found < 0:
                    break
                cursor = found + 1
                if found & 3 or found + len(needle) > len(region):
                    continue
                if all((struct.unpack_from("<I", region, found + index * 4)[0] & mask)
                       == expected[index]
                       for index, mask in enumerate(word_masks)):
                    hits.append(load.vaddr + found)
        return sorted(set(hits))

    def find_pointer_pair(self, first: int, second: int) -> list[int]:
        needle = struct.pack("<QQ", first, second)
        hits: list[int] = []
        for load in self.loads:
            if not (load.flags & PF_R) or (load.flags & PF_X):
                continue
            region = self.data[load.offset:load.offset + load.filesz]
            cursor = 0
            while True:
                found = region.find(needle, cursor)
                if found < 0:
                    break
                hits.append(load.vaddr + found - SETTER_SLOT)
                cursor = found + 1
        return sorted(set(hits))

    def find_pointer_sequence(self, values: tuple[int, ...],
                              address_point_slot: int) -> list[int]:
        if not values:
            return []
        needle = struct.pack("<" + "Q" * len(values), *values)
        hits: list[int] = []
        for load in self.loads:
            if not (load.flags & PF_R) or (load.flags & PF_X):
                continue
            region = self.data[load.offset:load.offset + load.filesz]
            cursor = 0
            while True:
                found = region.find(needle, cursor)
                if found < 0:
                    break
                location = load.vaddr + found
                if location >= address_point_slot:
                    hits.append(location - address_point_slot)
                cursor = found + 1
        return sorted(set(hits))

    def executable_rva(self, rva: int, size: int = 1) -> bool:
        return any(
            load.flags & PF_X and load.vaddr <= rva and
            rva + size <= load.vaddr + load.filesz
            for load in self.loads
        )

    def pointer_occurrence_index(self, wanted: set[int]) -> dict[int, list[int]]:
        result: dict[int, list[int]] = collections.defaultdict(list)
        if not wanted:
            return result
        for load in self.loads:
            if not (load.flags & PF_R) or (load.flags & PF_X):
                continue
            end = load.offset + load.filesz - (load.filesz % 8)
            for position in range(load.offset, end, 8):
                value = struct.unpack_from("<Q", self.data, position)[0]
                if value in wanted:
                    result[value].append(load.vaddr + position - load.offset)
        return result

    def direct_bl_callers(self, target: int) -> list[int]:
        callers: list[int] = []
        for load in self.loads:
            if not (load.flags & PF_X):
                continue
            start = load.offset
            end = load.offset + load.filesz - 3
            for position in range(start, end, 4):
                word = struct.unpack_from("<I", self.data, position)[0]
                if word >> 26 != 0b100101:
                    continue
                immediate = word & 0x03FFFFFF
                if immediate & (1 << 25):
                    immediate -= 1 << 26
                caller = load.vaddr + position - load.offset
                if caller + (immediate << 2) == target:
                    callers.append(caller)
        return callers


def choose_group_delta(reference: Elf, candidate: Elf,
                       roles: dict[str, int]) -> tuple[int, dict[str, list[int]]]:
    hits: dict[str, list[int]] = {}
    unique_deltas: list[int] = []
    for name, rva in roles.items():
        size = ROLE_SIGNATURE_SIZES.get(name, SIGNATURE_SIZE)
        signature = reference.bytes_at_rva(rva, size)
        masks = ROLE_WORD_MASKS.get(name)
        role_hits = (candidate.find_masked_in_executable(signature, masks)
                     if masks else candidate.find_in_executable(signature))
        hits[name] = role_hits
        if len(role_hits) == 1:
            unique_deltas.append(role_hits[0] - rva)
    if not unique_deltas:
        raise ValueError("semantic group has no unique signature anchor")
    delta, count = collections.Counter(unique_deltas).most_common(1)[0]
    if count < 1:
        raise ValueError("semantic group delta is ambiguous")
    return delta, hits


def select_role_hit(name: str, hits: list[int], expected: int) -> int:
    if expected in hits:
        return expected
    nearby = [hit for hit in hits if abs(hit - expected) <= MAX_ROLE_LOCAL_DRIFT]
    if len(nearby) != 1:
        raise ValueError(
            f"{name} has no unique hit near group RVA 0x{expected:x}"
        )
    return nearby[0]


def map_reference_pointer(reference: Elf, candidate: Elf, old_pointer: int,
                          deltas: set[int]) -> int | None:
    if not reference.executable_rva(old_pointer, SIGNATURE_SIZE):
        return None
    signature = reference.bytes_at_rva(old_pointer, SIGNATURE_SIZE)
    matches: list[int] = []
    for delta in deltas:
        mapped = old_pointer + delta
        try:
            if (candidate.executable_rva(mapped, SIGNATURE_SIZE) and
                    candidate.bytes_at_rva(mapped, SIGNATURE_SIZE) == signature):
                matches.append(mapped)
        except ValueError:
            continue
    unique = sorted(set(matches))
    return unique[0] if len(unique) == 1 else None


def resolve_vtables(reference: Elf, candidate: Elf, deltas: dict[str, int],
                    adjusted_candidate: int) -> dict[str, object]:
    pointer_deltas = set(deltas.values())
    mapped_by_role: dict[str, list[tuple[int, int, int]]] = {}
    pointer_cache: dict[int, int | None] = {}
    for role, (reference_rva, _minimum) in VTABLE_ROLES.items():
        offset = reference.rva_to_offset(reference_rva, VTABLE_SCAN_SIZE)
        slots: list[tuple[int, int, int]] = []
        for slot in range(0, VTABLE_SCAN_SIZE, 8):
            old_pointer = struct.unpack_from("<Q", reference.data, offset + slot)[0]
            if old_pointer not in pointer_cache:
                pointer_cache[old_pointer] = map_reference_pointer(
                    reference, candidate, old_pointer, pointer_deltas
                )
            mapped = pointer_cache[old_pointer]
            if mapped is not None:
                slots.append((slot, old_pointer, mapped))
        mapped_by_role[role] = slots

    wanted = {
        mapped for slots in mapped_by_role.values()
        for _slot, _old, mapped in slots
    }
    occurrences = candidate.pointer_occurrence_index(wanted)
    results: dict[str, object] = {}
    selected: dict[str, int] = {}
    candidate_sets: dict[str, list[int]] = {}
    for role, (reference_rva, minimum) in VTABLE_ROLES.items():
        slots = mapped_by_role[role]
        votes: collections.Counter[int] = collections.Counter()
        for slot, _old, mapped in slots:
            for location in occurrences.get(mapped, []):
                votes[location - slot] += 1
        if not votes:
            raise ValueError(f"{role} vtable has no structural candidate")
        best_score = votes.most_common(1)[0][1]
        best = sorted(base for base, score in votes.items() if score == best_score)
        if best_score < minimum:
            raise ValueError(
                f"{role} vtable score {best_score} is below {minimum}"
            )
        candidate_sets[role] = best

        chosen: int | None = best[0] if len(best) == 1 else None
        if chosen is None and role in ("physics_implementation", "vehicle_source"):
            expected = adjusted_candidate + (
                reference_rva - REFERENCE_ADJUSTED_VTABLE_RVA
            )
            if expected in best:
                chosen = expected
        if chosen is None and role == "native_physics_body" and "physics_backend" in selected:
            backend_delta = selected["physics_backend"] - VTABLE_ROLES[
                "physics_backend"
            ][0]
            chosen = min(best, key=lambda value: abs(
                (value - reference_rva) - backend_delta
            ))
            nearest_error = abs((chosen - reference_rva) - backend_delta)
            if nearest_error > 0x1000:
                chosen = None
        if chosen is None:
            # physics_backend is processed after native in the declaration;
            # defer that one ambiguity until all unique roles are known.
            candidate_sets[role] = best
            continue
        selected[role] = chosen
        results[role] = {
            "reference_rva": reference_rva,
            "candidate_rva": chosen,
            "mapped_slots": len(slots),
            "matched_slots": best_score,
            "top_candidate_count": len(best),
        }
        if len(best) > 1 and role in ("physics_implementation", "vehicle_source"):
            # These address-points are selected from a structurally tied set by
            # their exact delta from the independently resolved adjusted-setter
            # table.  Preserve that proof in the published profile instead of
            # making downstream consumers infer why the candidate was chosen.
            results[role]["related_to"] = "adjusted_setter_vtable"
            results[role]["relation_delta_error"] = 0

    if "native_physics_body" not in selected:
        best = candidate_sets["native_physics_body"]
        if "physics_backend" not in selected:
            raise ValueError("native-physics vtable cannot be related to backend")
        reference_rva = VTABLE_ROLES["native_physics_body"][0]
        backend_delta = selected["physics_backend"] - VTABLE_ROLES[
            "physics_backend"
        ][0]
        chosen = min(best, key=lambda value: abs(
            (value - reference_rva) - backend_delta
        ))
        error = abs((chosen - reference_rva) - backend_delta)
        if error > 0x1000:
            raise ValueError("native-physics/backend data delta is inconsistent")
        selected["native_physics_body"] = chosen
        results["native_physics_body"] = {
            "reference_rva": reference_rva,
            "candidate_rva": chosen,
            "mapped_slots": len(mapped_by_role["native_physics_body"]),
            "matched_slots": max(
                collections.Counter(
                    location - slot
                    for slot, _old, mapped in mapped_by_role["native_physics_body"]
                    for location in occurrences.get(mapped, [])
                ).values()
            ),
            "top_candidate_count": len(best),
            "related_to": "physics_backend",
            "delta_error": error,
        }

    # Some interface address-points intentionally share the same callable
    # slots, so slot voting alone yields the same small candidate set for more
    # than one role.  Resolve those identities by preserving their relation to
    # the nearest already-unique vtable anchor.  Requiring the predicted RVA to
    # be an exact member of the top-scoring set keeps this structural: no
    # package/build/channel literal is involved and a changed layout fails
    # closed instead of selecting the merely nearest table.
    pending_related = {
        role for role in VTABLE_ROLES
        if (role.startswith("physics_interface_") or role == "step_options")
        and role not in selected
    }
    while pending_related:
        progressed = False
        for role in sorted(pending_related):
            reference_rva = VTABLE_ROLES[role][0]
            anchors = [
                (abs(reference_rva - VTABLE_ROLES[anchor][0]), anchor)
                for anchor in selected
                if anchor in VTABLE_ROLES
            ]
            if not anchors:
                continue
            _distance, anchor = min(anchors)
            anchor_reference = VTABLE_ROLES[anchor][0]
            expected = reference_rva + selected[anchor] - anchor_reference
            best = candidate_sets[role]
            if expected not in best:
                continue
            selected[role] = expected
            results[role] = {
                "reference_rva": reference_rva,
                "candidate_rva": expected,
                "mapped_slots": len(mapped_by_role[role]),
                "matched_slots": max(
                    collections.Counter(
                        location - slot
                        for slot, _old, mapped in mapped_by_role[role]
                        for location in occurrences.get(mapped, [])
                    ).values()
                ),
                "top_candidate_count": len(best),
                "related_to": anchor,
                "relation_delta_error": 0,
            }
            pending_related.remove(role)
            progressed = True
            break
        if not progressed:
            break

    missing = sorted(set(VTABLE_ROLES) - set(selected))
    if missing:
        details = {
            role: [f"0x{value:x}" for value in candidate_sets.get(role, [])]
            for role in missing
        }
        resolved_details = {
            role: f"0x{value:x}" for role, value in selected.items()
            if role.startswith("physics_interface_")
        }
        raise ValueError(
            f"unresolved vtable roles: {details}; "
            f"resolved interface roles: {resolved_details}"
        )
    return results


def branch_normalized_context_matches(reference: Elf, candidate: Elf,
                                      reference_callsite: int,
                                      candidate_callsite: int) -> bool:
    # Compare four instructions before through four after.  Direct BL
    # immediates naturally change when independently linked regions move; the
    # opcode and every non-BL instruction must remain exact.
    for relative in range(-16, 20, 4):
        old = struct.unpack(
            "<I", reference.bytes_at_rva(reference_callsite + relative, 4)
        )[0]
        new = struct.unpack(
            "<I", candidate.bytes_at_rva(candidate_callsite + relative, 4)
        )[0]
        if old >> 26 == 0b100101 and new >> 26 == 0b100101:
            continue
        if old != new:
            return False
    return True


def resolve_frame_event_scheduler_return(reference: Elf, candidate: Elf,
                                         reference_target: int,
                                         candidate_target: int) -> dict[str, int]:
    reference_callsite = REFERENCE_FRAME_EVENT_SCHEDULER_RETURN_RVA - 4
    reference_callers = reference.direct_bl_callers(reference_target)
    candidate_callers = candidate.direct_bl_callers(candidate_target)
    if reference_callsite not in reference_callers:
        raise ValueError("reference scheduler return is not a FrameEvent caller")
    index = reference_callers.index(reference_callsite)
    if len(reference_callers) != len(candidate_callers) or index >= len(candidate_callers):
        raise ValueError("FrameEvent caller topology changed")
    candidate_callsite = candidate_callers[index]
    if not branch_normalized_context_matches(
            reference, candidate, reference_callsite, candidate_callsite):
        raise ValueError("FrameEvent scheduler caller context changed")
    return {
        "reference_return_rva": REFERENCE_FRAME_EVENT_SCHEDULER_RETURN_RVA,
        "candidate_return_rva": candidate_callsite + 4,
        "caller_index": index,
        "caller_count": len(candidate_callers),
        "normalized_context_instructions": 9,
    }


def resolve_lifecycle_vtables(reference: Elf, candidate: Elf,
                              selected: dict[str, int]) -> dict[str, object]:
    slot = 0x1D8
    reference_tables = sorted(set(
        reference.find_pointer_sequence((
            ROLE_GROUPS["physics_submit"]["lifecycle_phase_gate"],
            ROLE_GROUPS["physics_submit"]["lifecycle_shared_enter"],
        ), slot) +
        reference.find_pointer_sequence((
            ROLE_GROUPS["physics_submit"]["lifecycle_phase_gate"],
            ROLE_GROUPS["physics_submit"]["lifecycle_derived_enter"],
        ), slot)
    ))
    candidate_tables = sorted(set(
        candidate.find_pointer_sequence((
            selected["lifecycle_phase_gate"],
            selected["lifecycle_shared_enter"],
        ), slot) +
        candidate.find_pointer_sequence((
            selected["lifecycle_phase_gate"],
            selected["lifecycle_derived_enter"],
        ), slot)
    ))
    if not reference_tables or len(reference_tables) != len(candidate_tables):
        raise ValueError("lifecycle vtable topology changed")
    return {
        "phase_gate_slot": slot,
        "phase_enter_slot": slot + 8,
        "state_offset": 0x2D8,
        "countdown_state": 2,
        "reference_rvas": reference_tables,
        "candidate_rvas": candidate_tables,
        "candidate_count": len(candidate_tables),
    }


def structural_vtable_score(reference: Elf, candidate: Elf,
                            reference_rva: int, candidate_rva: int,
                            code_delta: int) -> dict[str, int | float]:
    reference_offset = reference.rva_to_offset(reference_rva, VTABLE_SIZE)
    candidate_offset = candidate.rva_to_offset(candidate_rva, VTABLE_SIZE)
    pointer_pairs = expected_delta = zero_pairs = 0
    for index in range(0, VTABLE_SIZE, 8):
        old = struct.unpack_from("<Q", reference.data, reference_offset + index)[0]
        new = struct.unpack_from("<Q", candidate.data, candidate_offset + index)[0]
        if old == 0 and new == 0:
            zero_pairs += 1
        if (0x1000 <= old < reference.executable_end and
                0x1000 <= new < candidate.executable_end):
            pointer_pairs += 1
            if new - old == code_delta:
                expected_delta += 1
    ratio = expected_delta / pointer_pairs if pointer_pairs else 0.0
    return {
        "pointer_pairs": pointer_pairs,
        "expected_delta_pairs": expected_delta,
        "expected_delta_ratio": round(ratio, 6),
        "zero_pairs": zero_pairs,
    }


def resolve(reference: Elf, candidate: Elf) -> dict[str, object]:
    role_results: dict[str, object] = {}
    deltas: dict[str, int] = {}
    selected: dict[str, int] = {}
    for group, roles in ROLE_GROUPS.items():
        delta, hits = choose_group_delta(reference, candidate, roles)
        deltas[group] = delta
        for name, reference_rva in roles.items():
            expected = reference_rva + delta
            role_hits = hits[name]
            selected_rva = select_role_hit(name, role_hits, expected)
            selected[name] = selected_rva
            role_results[name] = {
                "group": group,
                "reference_rva": reference_rva,
                "candidate_rva": selected_rva,
                "signature_bytes": ROLE_SIGNATURE_SIZES.get(
                    name, SIGNATURE_SIZE
                ),
                "all_hit_count": len(role_hits),
            }

    reference_vtables = reference.find_pointer_pair(
        ROLE_GROUPS["gameplay"]["adjusted_brake_setter"],
        ROLE_GROUPS["gameplay"]["adjusted_steering_setter"],
    )
    candidate_vtables = candidate.find_pointer_pair(
        selected["adjusted_brake_setter"],
        selected["adjusted_steering_setter"],
    )
    if REFERENCE_ADJUSTED_VTABLE_RVA not in reference_vtables:
        raise ValueError("reference adjusted vtable is not in structural candidates")
    reference_index = reference_vtables.index(REFERENCE_ADJUSTED_VTABLE_RVA)
    if len(reference_vtables) != len(candidate_vtables) or (
            reference_index >= len(candidate_vtables)):
        raise ValueError("adjusted-vtable candidate topology changed")
    candidate_vtable = candidate_vtables[reference_index]
    vtable_score = structural_vtable_score(
        reference, candidate, REFERENCE_ADJUSTED_VTABLE_RVA,
        candidate_vtable, deltas["gameplay"],
    )
    if (vtable_score["pointer_pairs"] < 100 or
            vtable_score["expected_delta_ratio"] < 0.85):
        raise ValueError("adjusted-vtable structural score is too weak")

    vtable_results = resolve_vtables(
        reference, candidate, deltas, candidate_vtable
    )
    scheduler_relationship = resolve_frame_event_scheduler_return(
        reference, candidate,
        ROLE_GROUPS["physics_submit"]["frame_event"],
        selected["frame_event"],
    )
    lifecycle_relationship = resolve_lifecycle_vtables(
        reference, candidate, selected
    )

    return {
        "schema": "A9_BUILD_PROFILE_RESOLUTION_V1",
        "channel_specific_literals": 0,
        "reference": {
            "path": str(reference.path),
            "sha256": reference.sha256,
            "build_id": reference.build_id,
        },
        "candidate": {
            "path": str(candidate.path),
            "sha256": candidate.sha256,
            "build_id": candidate.build_id,
            "image_size": max(
                load.vaddr + load.memsz for load in candidate.loads
            ),
        },
        "group_deltas": {name: f"0x{value:x}" for name, value in deltas.items()},
        "roles": role_results,
        "adjusted_setter_vtable": {
            "reference_rva": REFERENCE_ADJUSTED_VTABLE_RVA,
            "candidate_rva": candidate_vtable,
            "candidate_count": len(candidate_vtables),
            "reference_index": reference_index,
            "structural_score": vtable_score,
        },
        "vtables": vtable_results,
        "relationships": {
            "frame_event_scheduler_return": scheduler_relationship,
            "lifecycle_vtables": lifecycle_relationship,
        },
        "status": "STATIC_STAGE2_PASS_LIVE_READONLY_REQUIRED",
        "write_authorized": False,
        "pending": [
            "live_object_graph_readback",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-elf", type=pathlib.Path, required=True)
    parser.add_argument("--candidate-elf", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        report = resolve(Elf(args.reference_elf), Elf(args.candidate_elf))
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            temporary = args.output.with_name(args.output.name + ".tmp")
            temporary.write_text(rendered, encoding="utf-8", newline="\n")
            os.replace(temporary, args.output)
        print(rendered, end="")
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(json.dumps({
            "schema": "A9_BUILD_PROFILE_RESOLUTION_V1",
            "status": "FAIL",
            "write_authorized": False,
            "error": str(error),
        }, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
