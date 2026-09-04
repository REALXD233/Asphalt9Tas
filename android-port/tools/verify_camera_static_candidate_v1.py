#!/usr/bin/env python3
"""Verify the pinned offline Android camera-replay static candidate.

This verifier is read-only.  It hashes local binary regions and checks the
exact AArch64 instruction witnesses behind the RaceView blended-camera route
and the canonical camera-state layout.  It never invokes ADB or a live game.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


SCHEMA = "a9tas-camera-static-candidate-v1"
STATUS = "offline-static-candidate"


def parse_int(value: str) -> int:
    return int(value, 0)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    android_port = Path(__file__).resolve().parents[1]
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
        default=android_port / "baselines" / "camera_static_candidate_v1.json",
    )
    parser.add_argument("binary", nargs="?", type=Path)
    args = parser.parse_args()

    manifest = args.manifest.resolve()
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("schema") != SCHEMA:
        raise ValueError(f"unexpected schema: {data.get('schema')!r}")
    if data.get("status") != STATUS:
        raise ValueError("camera candidate must remain offline-only")

    binary_entry = data.get("binary", {})
    binary = (
        args.binary.resolve()
        if args.binary is not None
        else (android_port / binary_entry.get("path", "")).resolve()
    )
    if not binary.is_file():
        raise FileNotFoundError(f"missing Android game library: {binary}")
    expected_binary_hash = binary_entry.get("sha256", "").lower()
    if sha256_file(binary) != expected_binary_hash:
        raise ValueError("Android game library hash mismatch")

    blob = binary.read_bytes()
    region_count = 0
    for region in data.get("regions", []):
        offset = parse_int(region["rva"])
        size = parse_int(region["size"])
        if offset < 0 or size <= 0 or offset + size > len(blob):
            raise ValueError(f"invalid region bounds: {region!r}")
        actual = sha256_bytes(blob[offset : offset + size])
        if actual != region["sha256"].lower():
            raise ValueError(
                f"camera region hash mismatch: {region['role']} "
                f"expected={region['sha256']} actual={actual}"
            )
        region_count += 1

    witness_count = 0
    for witness in data.get("instruction_witnesses", []):
        offset = parse_int(witness["rva"])
        expected = parse_int(witness["word_le"])
        if offset < 0 or offset + 4 > len(blob):
            raise ValueError(f"invalid instruction RVA: {witness['rva']}")
        actual = struct.unpack_from("<I", blob, offset)[0]
        if actual != expected:
            raise ValueError(
                f"instruction witness mismatch at {witness['rva']}: "
                f"expected={expected:#010x} actual={actual:#010x}"
            )
        witness_count += 1

    route = data.get("static_route", {})
    state = route.get("canonical_state_offsets_from_camera_controller", {})
    expected_offsets = {
        "fov_candidate": "0xf0",
        "aspect_candidate": "0xf4",
        "position_xyz": "0xf8",
        "rotation_xyzw": "0x104",
    }
    if state != expected_offsets:
        raise ValueError(f"canonical camera-state layout drifted: {state!r}")
    policy = data.get("live_policy", {})
    if policy.get("writes_allowed") is not False:
        raise ValueError("offline camera candidate unexpectedly permits writes")
    if policy.get("vehicle_derived_camera_allowed") is not False:
        raise ValueError("vehicle-derived camera substitutes are forbidden")
    if policy.get("gameplay_camera_hook_allowed") is not False:
        raise ValueError("unproven gameplay camera hook must remain disabled")
    retired = data.get("interpretation", {}).get("retired_false_candidate", "")
    if "ReplayUserCameraController" not in retired or "must not" not in retired:
        raise ValueError("false-candidate retirement evidence is missing")

    print(
        "CAMERA_STATIC_CANDIDATE passed=1 offline_candidate=1 "
        f"regions={region_count} instructions={witness_count} "
        "active_route=1 canonical_state=1 false_candidate_retired=1 "
        "live_proven=0 writes=0 device_access=0 deployed=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
