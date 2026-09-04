#!/usr/bin/env python3
"""Compile a static A9 build-resolution JSON into the G8 binary ABI.

This tool performs no device access.  It deliberately preserves
``write_authorized=false`` semantics: the resulting blob describes one native
library build, while a separate live read-only receipt must authorize a race
session and supply lifecycle-owned object addresses.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import tempfile
from pathlib import Path
from typing import Any


MAGIC = b"A9BPR1\0\0"
VERSION = 1
SIZE = 384
ANNEX_MAGIC = b"A9BPAX1\0"
ANNEX_VERSION = 1
ANNEX_HEADER_SIZE = 32
VEHICLE_RVA_COUNT = 42
REQUIRED_FLAGS = 0x1F
EXPECTED_SCHEMA = "A9_BUILD_PROFILE_RESOLUTION_V1"
EXPECTED_STATUS = "STATIC_STAGE2_PASS_LIVE_READONLY_REQUIRED"

HOOK_ROLES = (
    "physics_interval",
    "final_writer",
    "frame_event",
    "nitro_state",
    "physics_submit",
    "barrel_roll_tail",
    "barrel_yaw_tail",
    "logic_dispatcher",
)
VTABLE_ROLES = (
    "main_time_source",
    "embedded_time_source",
    "physics_context",
    "physics_implementation",
    "step_options",
    "native_physics_body",
    "nitro_service",
)
SETTER_ROLES = ("adjusted_brake_setter", "adjusted_steering_setter")
LIFECYCLE_ROLES = (
    "lifecycle_phase_gate",
    "lifecycle_shared_enter",
    "lifecycle_derived_enter",
    "lifecycle_racing_store",
)
VEHICLE_WRAPPER_ROLES = tuple(
    f"vehicle_wrapper_slot{suffix}"
    for suffix in ("40", "48", "58", "60", "68", "88", "90", "98", "A0")
)
VEHICLE_DELEGATE_ROLES = tuple(
    f"vehicle_delegate_slot{suffix}"
    for suffix in ("40", "48", "58", "60", "68", "88", "90", "98", "A0")
)
PHYSICS_API_ROLES = (
    "physics_set_pose",
    "physics_set_position",
    "physics_set_rotation",
    "physics_set_linear",
    "physics_set_angular",
    "physics_get_linear",
    "physics_get_angular",
)


class ProfileError(ValueError):
    """The resolver JSON cannot safely produce a runtime profile."""


def _mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ProfileError(f"{name} must be an object")
    return value


def _integer(value: Any, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ProfileError(f"{name} must be an integer")
    if value < 0 or value > 0xFFFFFFFFFFFFFFFF:
        raise ProfileError(f"{name} is outside uint64")
    return value


def _rva(value: Any, name: str, image_size: int) -> int:
    result = _integer(value, name)
    if result < 0x1000 or result >= image_size or result & 3:
        raise ProfileError(f"{name} is not a valid aligned image RVA")
    return result


def _hex_bytes(value: Any, size: int, name: str) -> bytes:
    if not isinstance(value, str) or len(value) != size * 2:
        raise ProfileError(f"{name} must contain {size * 2} hex characters")
    try:
        result = bytes.fromhex(value)
    except ValueError as error:
        raise ProfileError(f"{name} is not valid hex") from error
    if len(result) != size or not any(result):
        raise ProfileError(f"{name} must be nonzero and {size} bytes")
    return result


def compile_profile(document: dict[str, Any], source_bytes: bytes) -> bytes:
    if document.get("schema") != EXPECTED_SCHEMA:
        raise ProfileError("unexpected resolver schema")
    if document.get("status") != EXPECTED_STATUS:
        raise ProfileError("static resolver stage has not passed")
    if document.get("write_authorized") is not False:
        raise ProfileError("resolver JSON must remain write_authorized=false")
    if document.get("channel_specific_literals") != 0:
        raise ProfileError("resolver JSON contains channel-specific literals")
    if document.get("pending") != ["live_object_graph_readback"]:
        raise ProfileError("resolver JSON must require the live object-graph Gate")

    candidate = _mapping(document.get("candidate"), "candidate")
    roles = _mapping(document.get("roles"), "roles")
    vtables = _mapping(document.get("vtables"), "vtables")
    relationships = _mapping(document.get("relationships"), "relationships")
    group_deltas = _mapping(document.get("group_deltas"), "group_deltas")
    adjusted = _mapping(
        document.get("adjusted_setter_vtable"), "adjusted_setter_vtable"
    )

    image_size = _integer(candidate.get("image_size"), "candidate.image_size")
    if image_size < 0x100000:
        raise ProfileError("candidate.image_size is implausibly small")

    def role_rva(name: str) -> int:
        role = _mapping(roles.get(name), f"roles.{name}")
        hit_count = _integer(role.get("all_hit_count"), f"roles.{name}.all_hit_count")
        group = role.get("group")
        if hit_count < 1 or not isinstance(group, str) or group not in group_deltas:
            raise ProfileError(f"roles.{name} lacks a resolved signature group")
        return _rva(role.get("candidate_rva"), f"roles.{name}.candidate_rva", image_size)

    def vtable_rva(name: str) -> int:
        role = _mapping(vtables.get(name), f"vtables.{name}")
        if _integer(role.get("matched_slots"), f"vtables.{name}.matched_slots") <= 0:
            raise ProfileError(f"vtables.{name} has no structural matches")
        if _integer(role.get("top_candidate_count"), f"vtables.{name}.top_candidate_count") > 1:
            exact_relation = (
                "related_to" in role and role.get("relation_delta_error") == 0
            )
            bounded_backend_relation = (
                role.get("related_to") == "physics_backend"
                and isinstance(role.get("delta_error"), int)
                and 0 <= role["delta_error"] <= 0x1000
            )
            if not exact_relation and not bounded_backend_relation:
                raise ProfileError(f"vtables.{name} remains structurally ambiguous")
        return _rva(
            role.get("candidate_rva"), f"vtables.{name}.candidate_rva", image_size
        )

    hook_rvas = [role_rva(name) for name in HOOK_ROLES]
    resolved_vtables = {name: vtable_rva(name) for name in VTABLE_ROLES}
    nitro_dispatch = role_rva("nitro_dispatch")
    setter_rvas = [role_rva(name) for name in SETTER_ROLES]
    lifecycle_rvas = [role_rva(name) for name in LIFECYCLE_ROLES]
    adjusted_vtable = _rva(
        adjusted.get("candidate_rva"),
        "adjusted_setter_vtable.candidate_rva",
        image_size,
    )
    frame_relationship = _mapping(
        relationships.get("frame_event_scheduler_return"),
        "relationships.frame_event_scheduler_return",
    )
    frame_return = _rva(
        frame_relationship.get("candidate_return_rva"),
        "relationships.frame_event_scheduler_return.candidate_return_rva",
        image_size,
    )

    native_sha = _hex_bytes(candidate.get("sha256"), 32, "candidate.sha256")
    build_id = _hex_bytes(candidate.get("build_id"), 20, "candidate.build_id")
    profile_sha = hashlib.sha256(source_bytes).digest()

    # PhysicsImplementation and CarPhysicsBodySource are the same proven
    # identity in the frozen G8 runtime; retain both ABI fields explicitly.
    qwords = [
        *hook_rvas,
        resolved_vtables["main_time_source"],
        resolved_vtables["embedded_time_source"],
        resolved_vtables["physics_context"],
        resolved_vtables["physics_implementation"],
        resolved_vtables["step_options"],
        resolved_vtables["native_physics_body"],
        resolved_vtables["nitro_service"],
        nitro_dispatch,
        vtable_rva("vehicle_source"),
        resolved_vtables["physics_implementation"],
        adjusted_vtable,
        *setter_rvas,
        frame_return,
        *lifecycle_rvas,
        role_rva("barrel_random_bool"),
        role_rva("barrel_random_lerp"),
    ]
    if len(qwords) != 28:
        raise AssertionError("internal G8 profile qword count drift")
    blob = b"".join(
        (
            struct.pack("<8sIIIIQ", MAGIC, VERSION, SIZE, REQUIRED_FLAGS, 0, image_size),
            struct.pack("<28Q", *qwords),
            native_sha,
            profile_sha,
            build_id,
            bytes(44),
        )
    )
    if len(blob) != SIZE:
        raise AssertionError(f"internal G8 profile size drift: {len(blob)}")

    physics_interfaces = [vtable_rva(f"physics_interface_{index}")
                          for index in range(9)]
    vehicle_qwords = [
        *physics_interfaces,
        role_rva("vehicle_position_getter"),
        role_rva("vehicle_rotation_getter"),
        *(role_rva(name) for name in VEHICLE_WRAPPER_ROLES),
        *(role_rva(name) for name in VEHICLE_DELEGATE_ROLES),
        vtable_rva("vehicle_source"),
        role_rva("vehicle_source_update"),
        resolved_vtables["physics_implementation"],
        role_rva("car_physics_body_update"),
        vtable_rva("physics_backend"),
        resolved_vtables["native_physics_body"],
        *(role_rva(name) for name in PHYSICS_API_ROLES),
    ]
    if len(vehicle_qwords) != VEHICLE_RVA_COUNT:
        raise AssertionError("internal vehicle resolver profile count drift")
    lifecycle_relationship = _mapping(
        relationships.get("lifecycle_vtables"),
        "relationships.lifecycle_vtables",
    )
    lifecycle_values = lifecycle_relationship.get("candidate_rvas")
    if not isinstance(lifecycle_values, list) or not lifecycle_values:
        raise ProfileError("lifecycle vtable candidate list is empty")
    lifecycle_qwords = [
        _rva(value, f"lifecycle_vtables[{index}]", image_size)
        for index, value in enumerate(lifecycle_values)
    ]
    if lifecycle_qwords != sorted(set(lifecycle_qwords)):
        raise ProfileError("lifecycle vtable candidate list is not sorted unique")
    if len(lifecycle_qwords) > 4096:
        raise ProfileError("lifecycle vtable candidate list exceeds ABI bound")
    total_size = (SIZE + ANNEX_HEADER_SIZE +
                  8 * (len(vehicle_qwords) + len(lifecycle_qwords)))
    annex = b"".join((
        struct.pack(
            "<8sIIIIII",
            ANNEX_MAGIC,
            ANNEX_VERSION,
            ANNEX_HEADER_SIZE,
            len(vehicle_qwords),
            len(lifecycle_qwords),
            total_size,
            0,
        ),
        struct.pack(f"<{len(vehicle_qwords)}Q", *vehicle_qwords),
        struct.pack(f"<{len(lifecycle_qwords)}Q", *lifecycle_qwords),
    ))
    if len(blob) + len(annex) != total_size:
        raise AssertionError("internal G8 profile annex size drift")
    return blob + annex


def load_and_compile(path: Path) -> bytes:
    source_bytes = path.read_bytes()
    try:
        document = json.loads(source_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProfileError("profile source is not valid UTF-8 JSON") from error
    return compile_profile(_mapping(document, "root"), source_bytes)


def atomic_write(path: Path, blob: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(blob)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args(argv)
    blob = load_and_compile(args.source)
    atomic_write(args.output, blob)
    print(
        "G8_RUNTIME_BUILD_PROFILE_GENERATED "
        f"size={len(blob)} core_size={SIZE} "
        f"sha256={hashlib.sha256(blob).hexdigest()} "
        "device_access=0 write_authorized=0 channel_literals=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
