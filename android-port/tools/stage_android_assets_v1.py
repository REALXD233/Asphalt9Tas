"""Stage only manifest-referenced runtime assets; never mutate source assets."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path


def stage(source: Path, parent: Path, no_profiles: bool = False) -> Path:
    source = source.resolve()
    manifest_path = source / "runtime/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    required: dict[str, str] = {}
    artifacts = list(manifest["identity_helpers"])
    for backend in manifest["artifact_sets"]:
        artifacts.extend(backend["artifacts"])
    for artifact in artifacts:
        name, digest = artifact["asset"], artifact["sha256"]
        if not name.startswith("runtime/") or "\\" in name or ".." in Path(name).parts:
            raise ValueError(f"invalid runtime asset: {name}")
        if name in required and required[name] != digest:
            raise ValueError(f"conflicting runtime identity: {name}")
        required[name] = digest
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="apk-assets-", dir=parent))
    assets = root / "assets"
    (assets / "runtime").mkdir(parents=True)
    try:
        for child in source.iterdir():
            if child.name == "runtime" or (no_profiles and child.name == "profiles"):
                continue
            if child.is_dir():
                shutil.copytree(child, assets / child.name)
            else:
                shutil.copy2(child, assets / child.name)
        shutil.copy2(manifest_path, assets / "runtime/manifest.json")
        for name, digest in required.items():
            path = source / name
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise ValueError(f"runtime hash mismatch: {name}")
            destination = assets / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        if no_profiles:
            (assets / "profiles").mkdir()
            (assets / "profiles/registry.json").write_text(
                '{"schema":1,"profiles":[]}\n', encoding="utf-8")
        return root
    except BaseException:
        shutil.rmtree(root)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("parent", type=Path)
    parser.add_argument("--no-profiles", action="store_true")
    args = parser.parse_args()
    print(stage(args.source, args.parent, args.no_profiles))
