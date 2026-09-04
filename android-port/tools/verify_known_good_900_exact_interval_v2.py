#!/usr/bin/env python3
"""Verify the user-validated exact-interval 900-frame live baseline offline."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


SCHEMA = "a9tas-known-good-baseline-v2"
STATUS = "live-proven-user-validated"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_group(root: Path, entries: list[dict[str, str]], label: str) -> int:
    verified = 0
    roles: set[str] = set()
    paths: set[str] = set()
    for entry in entries:
        relative = entry.get("path", "")
        expected = entry.get("sha256", "").lower()
        role = entry.get("role", "")
        if not relative or len(expected) != 64:
            raise ValueError(f"invalid {label} entry: {entry!r}")
        if label == "runtime":
            if not role or role in roles:
                raise ValueError(f"invalid or duplicate runtime role: {role!r}")
            roles.add(role)
        if relative in paths:
            raise ValueError(f"duplicate {label} path: {relative}")
        paths.add(relative)
        candidate = (root / relative).resolve()
        if root not in candidate.parents:
            raise ValueError(f"{label} path escapes android-port: {relative}")
        if not candidate.is_file():
            raise FileNotFoundError(f"missing {label}: {relative}")
        actual = sha256(candidate)
        if actual != expected:
            raise ValueError(
                f"{label} hash mismatch: {relative} "
                f"expected={expected} actual={actual}"
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
        / "known_good_900_exact_interval_v2.json",
    )
    args = parser.parse_args()

    manifest = args.manifest.resolve()
    android_port = manifest.parents[1]
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("schema") != SCHEMA:
        raise ValueError(f"unexpected baseline schema: {data.get('schema')!r}")
    if data.get("status") != STATUS:
        raise ValueError("baseline is not marked live-proven and user-validated")

    environment = data.get("environment", {})
    if (
        environment.get("track") != "Ancient Ruins"
        or environment.get("car") != "Chevrolet Camaro ZL1"
        or environment.get("frames") != 900
        or environment.get("physics_interval_calls") != 900
        or environment.get("physics_interval_overrides") != 900
    ):
        raise ValueError("exact-interval environment identity mismatch")

    visible = data.get("user_visible_result", {})
    if (
        visible.get("vehicle_state") != "normal"
        or visible.get("camera_vs_record_only") != "improved"
    ):
        raise ValueError("user-visible validation is missing")

    live_doc = (android_port / data.get("live_pass_document", "")).resolve()
    expected_live_doc_hash = data.get("live_pass_sha256", "").lower()
    if (
        android_port not in live_doc.parents
        or not live_doc.is_file()
        or len(expected_live_doc_hash) != 64
        or sha256(live_doc) != expected_live_doc_hash
    ):
        raise ValueError("live-pass evidence document identity mismatch")

    runtime_entries = data.get("files", [])
    source_entries = data.get("source_snapshot", [])
    runtime_count = verify_group(android_port, runtime_entries, "runtime")
    source_count = verify_group(android_port, source_entries, "source snapshot")
    if runtime_count != 18 or source_count != 5:
        raise ValueError(
            f"baseline cardinality changed: runtime={runtime_count} "
            f"source={source_count}"
        )

    roles = {entry["role"]: entry for entry in runtime_entries}
    interval_source = roles.get("physics_interval_source", {})
    if (
        interval_source.get("sha256")
        != "329683a976ad7d0e3f7d326c567fbdfaf911be1fc6bca5573087de2e11bf6347"
    ):
        raise ValueError("authoritative 900-call interval source is not pinned")

    print(
        "KNOWN_GOOD_900_EXACT_INTERVAL_V2 passed=1 "
        "live_proven=1 user_validated=1 frames=900 interval_overrides=900 "
        f"runtime_files={runtime_count} source_files={source_count} "
        "device_access=0 deployed=0 rebuilt=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
