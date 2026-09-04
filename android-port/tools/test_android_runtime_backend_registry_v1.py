#!/usr/bin/env python3
"""Offline binding-policy gate for Android execution-backend selection."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "A9TasAndroid/app/src/main/assets"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def select(backends: list[dict], machine: str, bridge: str, libc_sha: str) -> dict | None:
    matches = [item for item in backends
               if item.get("enabled", True) and item.get("accepted", True)
               if item["host_machine"] == machine
               and item["bridge_set"] == bridge
               and (item.get("libc_binding", "exact") == "dynamic"
                    or item["libc_sha256"] == libc_sha)]
    return matches[0] if len(matches) == 1 else None


def main() -> int:
    manifest = json.loads((ASSETS / "runtime/manifest.json").read_text("utf-8"))
    require(manifest["schema"] == 2, "schema")
    backends = manifest["artifact_sets"]
    helpers = manifest["identity_helpers"]
    require({item["host_machine"] for item in helpers} == {"arm64", "x86_64"},
            "identity helper architectures")
    for helper in helpers:
        data = (ASSETS / helper["asset"]).read_bytes()
        require(hashlib.sha256(data).hexdigest() == helper["sha256"],
                f"identity helper drift: {helper['host_machine']}")
    require(backends, "empty registry")
    keys: set[tuple[str, str, str, str]] = set()
    ids: set[str] = set()
    for backend in backends:
        require(re.fullmatch(r"[A-Z0-9_]{4,64}", backend["carrier_receipt_prefix"]) is not None,
                "invalid carrier receipt prefix")
        require(backend.get("payload_staging") in {"local_tmp", "target_app_cache"},
                "invalid payload staging")
        require(backend["payload_staging"] != "target_app_cache" or
                backend["host_machine"] == "arm64",
                "private payload staging is ARM64-only")
        require(isinstance(backend.get("enabled", True), bool), "invalid enabled flag")
        require(isinstance(backend.get("accepted", True), bool), "invalid accepted flag")
        require(not backend.get("accepted", True) or backend.get("enabled", True),
                "accepted backend must be enabled")
        binding = backend.get("libc_binding", "exact")
        require(binding in {"exact", "dynamic"}, "invalid libc binding")
        require(binding != "dynamic" or
                (backend["host_machine"] == "arm64" and
                 backend["bridge_set"] == "none" and
                 backend["payload_staging"] == "target_app_cache"),
                "dynamic libc binding is restricted to native ARM64 private staging")
        require(backend["id"] not in ids, "duplicate backend id")
        ids.add(backend["id"])
        key = (backend["host_machine"], backend["bridge_set"],
               binding, backend["libc_sha256"] if binding == "exact" else "dynamic")
        require(key not in keys, "ambiguous environment key")
        keys.add(key)
        require(re.fullmatch(r"[0-9a-f]{64}", backend["libc_sha256"]) is not None,
                "invalid libc hash")
        artifacts = {item["device_name"]: item for item in backend["artifacts"]}
        roles = backend["roles"]
        require({"carrier", "controller", "observer", "payload"} <= roles.keys(),
                "missing mandatory role")
        require(set(roles.values()) <= artifacts.keys(), "missing role artifact")
        for item in artifacts.values():
            data = (ASSETS / item["asset"]).read_bytes()
            require(hashlib.sha256(data).hexdigest() == item["sha256"],
                    f"artifact drift: {item['device_name']}")

    proven = backends[0]
    chosen = select(backends, proven["host_machine"], proven["bridge_set"],
                    proven["libc_sha256"])
    require(chosen is proven, "exact backend did not select uniquely")
    require(select(backends, proven["host_machine"], "libhoudini.so",
                   proven["libc_sha256"]) is None, "bridge mismatch was accepted")
    require(select(backends, proven["host_machine"], proven["bridge_set"],
                   "0" * 64) is None, "libc mismatch was accepted")
    native_bundled = sum(item["host_machine"] == "arm64" for item in backends)
    native_enabled = sum(item["host_machine"] == "arm64"
                         and item.get("enabled", True)
                         and item.get("accepted", True) for item in backends)
    require(native_bundled == 1, "native ARM64 backend is not bundled")
    require(native_enabled == 1, "verified native ARM64 backend is not enabled")
    native = next(item for item in backends if item["host_machine"] == "arm64")
    require(native["payload_staging"] == "target_app_cache",
            "native ARM64 payload must use the target app cache")
    require(native.get("libc_binding") == "dynamic",
            "native ARM64 backend must declare dynamic libc binding")
    require(select(backends, "arm64", "none", native["libc_sha256"]) is native,
            "verified native ARM64 environment did not select")
    require(select(backends, "arm64", "none", "0" * 64) is native,
            "native ARM64 dynamic resolver still depends on a libc file hash")
    require(select(backends, "arm64", "libhoudini.so", "0" * 64) is None,
            "native ARM64 dynamic backend accepted a translation bridge")
    print("ANDROID_RUNTIME_BACKEND_REGISTRY passed=1 "
          f"backends={len(backends)} x86_exact_libc=1 arm64_dynamic_libc=1 bridge_bound=1 "
          f"native_arm64_bundled={native_bundled} native_arm64_enabled={native_enabled} "
          f"identity_helpers={len(helpers)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, OSError, ValueError) as error:
        print(f"ANDROID_RUNTIME_BACKEND_REGISTRY passed=0 error={error}")
        raise SystemExit(1)
