#!/usr/bin/env python3
"""Package a compiled G8 profile into the Android import bundle format."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import struct
import tempfile
from pathlib import Path


MAGIC = b"A9BPR1\0\0"
CORE_SIZE = 384


def exact_rva(text: str, image_size: int) -> str:
    value = int(text, 0)
    if value < 0x1000 or value >= image_size or value & 7:
        raise ValueError(f"invalid practice vptr RVA: {text}")
    return f"0x{value:x}"


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolution", required=True, type=Path)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--practice-vptr", action="append", required=True)
    args = parser.parse_args()

    if len(args.practice_vptr) != 5:
        raise ValueError("exactly five --practice-vptr values are required")
    resolution = json.loads(args.resolution.read_text(encoding="utf-8"))
    profile = args.profile.read_bytes()
    if len(profile) < CORE_SIZE or profile[:8] != MAGIC:
        raise ValueError("compiled profile has an invalid core")
    version, core_size = struct.unpack_from("<II", profile, 8)
    if version != 1 or core_size != CORE_SIZE:
        raise ValueError("compiled profile ABI is unsupported")
    image_size = struct.unpack_from("<Q", profile, 24)[0]
    native_sha = profile[256:288].hex()
    build_id = profile[320:340].hex()
    candidate = resolution.get("candidate", {})
    if native_sha != candidate.get("sha256") or build_id != candidate.get("build_id"):
        raise ValueError("compiled profile and resolution identity differ")

    names = ("vptr0_rva", "vptr588_rva", "vptr6c0_rva",
             "vptr718_rva", "vptr748_rva")
    proof = {
        name: exact_rva(value, image_size)
        for name, value in zip(names, args.practice_vptr)
    }
    document = {
        "schema": "A9_PROFILE_BUNDLE_V1",
        "label": args.label.strip(),
        "native_sha256": native_sha,
        "build_id": build_id,
        "profile_sha256": hashlib.sha256(profile).hexdigest(),
        "profile_base64": base64.b64encode(profile).decode("ascii"),
        "practice_proof": proof,
    }
    if not document["label"]:
        raise ValueError("label must not be empty")
    atomic_write(
        args.output,
        (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8"),
    )
    print(
        "A9_PROFILE_BUNDLE_PACKAGED_V1 "
        f"native_sha256={native_sha} profile_sha256={document['profile_sha256']} "
        f"size={args.output.stat().st_size}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
