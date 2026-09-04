#!/usr/bin/env python3
"""Verify the immutable, live-proven 900-frame regression baseline.

This verifier is deliberately offline.  It only reads local files and hashes;
it does not invoke ADB, deploy artifacts, or rebuild the proven executable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


SCHEMA = "a9tas-known-good-baseline-v1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_group(root: Path, entries: list[dict[str, str]], label: str) -> int:
    verified = 0
    for entry in entries:
        relative = entry.get("path", "")
        expected = entry.get("sha256", "").lower()
        if not relative or len(expected) != 64:
            raise ValueError(f"invalid {label} entry: {entry!r}")
        candidate = (root / relative).resolve()
        if root not in candidate.parents:
            raise ValueError(f"{label} path escapes android-port: {relative}")
        if not candidate.is_file():
            raise FileNotFoundError(f"missing {label}: {relative}")
        actual = sha256(candidate)
        if actual != expected:
            raise ValueError(
                f"{label} hash mismatch: {relative} expected={expected} actual={actual}"
            )
        verified += 1
    return verified


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1]
        / "baselines"
        / "known_good_900_v1.json",
    )
    args = parser.parse_args()

    manifest = args.manifest.resolve()
    android_port = manifest.parents[1]
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("schema") != SCHEMA:
        raise ValueError(f"unexpected baseline schema: {data.get('schema')!r}")
    if data.get("status") != "live-proven":
        raise ValueError("baseline is not marked live-proven")
    environment = data.get("environment", {})
    if environment.get("frames") != 900:
        raise ValueError("baseline frame count is not 900")

    live_doc = (android_port / data.get("live_pass_document", "")).resolve()
    if android_port not in live_doc.parents or not live_doc.is_file():
        raise FileNotFoundError("live-pass evidence document is missing")

    runtime_count = verify_group(android_port, data.get("files", []), "runtime")
    source_count = verify_group(
        android_port, data.get("source_snapshot", []), "source snapshot"
    )
    if runtime_count != 13 or source_count != 5:
        raise ValueError(
            f"baseline cardinality changed: runtime={runtime_count} source={source_count}"
        )

    print(
        "KNOWN_GOOD_900_BASELINE passed=1 live_proven=1 frames=900 "
        f"runtime_files={runtime_count} source_files={source_count} "
        "device_access=0 deployed=0 rebuilt=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
