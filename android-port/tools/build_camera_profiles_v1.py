#!/usr/bin/env python3
"""Build fixed-layout Camera Tool profiles from verified G8 profile bundles."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct


SIZE = 576
CORE_SIZE = 384
ANNEX_SIZE = 32
VEHICLE_SIZE = 42 * 8

CAMERA = {
    "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0": [
        0x8174F40, 0x8175108, 0x7EDF608, 0xA4F9680,
        0x8176180, 0x8176298, 0x9DF3FE8, 0x9D5B768,
        0x81766D8, 0x3A787D8, 0x7F0F8A8, 0x4D10D90,
        0x4BF4D28, 0x4BF4D7C, 0x4C711F8,
    ],
    "439fd7f94ef570d94d7673eedc1f5c0ff39ed0a0536f12b68e0d589c3eec6c71": [
        0x81ADBC8, 0x81ADD90, 0x7EDF608, 0,  # maximum filled from image size
        0x81AEE08, 0x81AEF20, 0x9E3BF30, 0x9DA36B0,
        0x81AF360, 0x3A9FCB0, 0x7F48530, 0x4D43BBC,
        0x4C27B54, 0x4C27BA8, 0x4CA4024,
    ],
}


def build(bundle_path: pathlib.Path, native_sha: str) -> bytes:
    bundle = bundle_path.read_bytes()
    if len(bundle) < CORE_SIZE + ANNEX_SIZE + VEHICLE_SIZE:
        raise ValueError(f"profile bundle is truncated: {bundle_path}")
    if bundle[:8] != b"A9BPR1\0\0":
        raise ValueError(f"bad G8 profile magic: {bundle_path}")
    image_size = struct.unpack_from("<Q", bundle, 24)[0]
    camera = list(CAMERA[native_sha])
    if camera[3] == 0:
        camera[3] = (image_size - 4) & ~3
    output = bytearray(SIZE)
    output[:8] = b"A9CPR1\0\0"
    struct.pack_into("<IIQ", output, 8, 1, SIZE, image_size)
    struct.pack_into("<15Q", output, 24, *camera)
    output[144:176] = bytes.fromhex(native_sha)
    vehicle_begin = CORE_SIZE + ANNEX_SIZE
    output[176:512] = bundle[vehicle_begin:vehicle_begin + VEHICLE_SIZE]
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--registry", type=pathlib.Path, required=True)
    parser.add_argument("--assets-root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    registry = json.loads(args.registry.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    emitted = []
    for entry in registry["profiles"]:
        native_sha = entry["native_sha256"].lower()
        if native_sha not in CAMERA:
            continue
        bundle_path = args.assets_root / entry["profile_asset"]
        payload = build(bundle_path, native_sha)
        name = native_sha[:16] + ".a9camera.bin"
        path = args.output / name
        path.write_bytes(payload)
        emitted.append({
            "id": entry["id"],
            "native_sha256": native_sha,
            "asset": "profiles/" + name,
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    if len(emitted) != len(CAMERA):
        raise ValueError("not every camera profile was emitted")
    (args.output / "registry.json").write_text(
        json.dumps({"schema": 1, "profiles": emitted}, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
