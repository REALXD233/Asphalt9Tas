#!/usr/bin/env python3
"""G9 command-line recording library for the canonical A9TAS1 container."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from a9tas_recording_v1 import (
    RecordingError, build_manifest, decode_a9g4r2, encode_archive,
    read_archive,
)


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.",
                                         suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _summary(path: Path) -> dict[str, Any]:
    archive = read_archive(path)
    manifest = archive.manifest
    return {
        "path": str(path),
        "recording_id": manifest["recording_id"],
        "title": manifest["title"],
        "created_utc": manifest["created_utc"],
        "package": manifest["game"]["package"],
        "map": manifest["race"]["map"],
        "car": manifest["race"]["car"],
        "control_mode": manifest["race"]["control_mode"],
        "frames": archive.source.frame_count,
        "intervals": archive.source.interval_count,
        "target_tick": manifest["recording"]["target_tick"],
        "recording_sha256": archive.recording_sha256,
    }


def _parse_target(text: str, frame_count: int) -> int:
    if text == "end":
        return frame_count - 1
    try:
        target = int(text, 10)
    except ValueError as error:
        raise RecordingError("target_tick.not_integer") from error
    if not 0 <= target < frame_count:
        raise RecordingError("target_tick.out_of_range")
    return target


def command_pack(args: argparse.Namespace) -> dict[str, Any]:
    recording = args.recording.read_bytes()
    source = decode_a9g4r2(recording)
    target = _parse_target(args.target_tick, source.frame_count)
    recording_id = args.recording_id or str(uuid.uuid4())
    created = args.created_utc or datetime.now(timezone.utc).isoformat(
        timespec="seconds").replace("+00:00", "Z")
    manifest = build_manifest(
        source=source, recording_id=recording_id, title=args.title,
        created_utc=created, game_package=args.game_package,
        game_version=args.game_version, native_sha256=args.native_sha256,
        build_id=args.build_id, build_profile_sha256=args.profile_sha256,
        map_name=args.map_name, car_name=args.car_name,
        control_mode=args.control_mode, target_tick=target, notes=args.notes,
    )
    blob = encode_archive(manifest, recording)
    _atomic_write(args.output, blob)
    return _summary(args.output)


def _rewrite(path: Path, mutate: Any) -> dict[str, Any]:
    archive = read_archive(path)
    manifest = json.loads(json.dumps(archive.manifest, ensure_ascii=False))
    mutate(manifest, archive.source.frame_count)
    _atomic_write(path, encode_archive(manifest, archive.recording))
    return _summary(path)


def command_rename(args: argparse.Namespace) -> dict[str, Any]:
    return _rewrite(args.archive,
                    lambda manifest, _: manifest.__setitem__("title", args.title))


def command_set_target(args: argparse.Namespace) -> dict[str, Any]:
    def mutate(manifest: dict[str, Any], frames: int) -> None:
        manifest["recording"]["target_tick"] = _parse_target(args.target_tick,
                                                               frames)
    return _rewrite(args.archive, mutate)


def command_unpack(args: argparse.Namespace) -> dict[str, Any]:
    archive = read_archive(args.archive)
    _atomic_write(args.output, archive.recording)
    return {"path": str(args.output), "bytes": len(archive.recording),
            "recording_sha256": archive.recording_sha256}


def command_list(args: argparse.Namespace) -> dict[str, Any]:
    if not args.library.exists():
        return {"library": str(args.library), "recordings": [], "invalid": []}
    recordings: list[dict[str, Any]] = []
    invalid: list[dict[str, str]] = []
    for path in sorted(args.library.glob("*.a9tas")):
        try:
            recordings.append(_summary(path))
        except RecordingError as error:
            invalid.append({"path": str(path), "error": str(error)})
    ids = [item["recording_id"] for item in recordings]
    if len(ids) != len(set(ids)):
        raise RecordingError("library.duplicate_recording_id")
    return {"library": str(args.library), "recordings": recordings,
            "invalid": invalid}


def command_delete(args: argparse.Namespace) -> dict[str, Any]:
    if not args.yes:
        raise RecordingError("delete.requires_yes")
    root = args.library.resolve(strict=True)
    target = args.archive.resolve(strict=True)
    if target.suffix.lower() != ".a9tas" or root not in target.parents:
        raise RecordingError("delete.outside_library")
    recording_id = read_archive(target).manifest["recording_id"]
    target.unlink()
    return {"deleted": str(target), "recording_id": recording_id}


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    pack = sub.add_parser("pack")
    pack.add_argument("recording", type=Path)
    pack.add_argument("output", type=Path)
    pack.add_argument("--title", required=True)
    pack.add_argument("--recording-id")
    pack.add_argument("--created-utc")
    pack.add_argument("--game-package", required=True)
    pack.add_argument("--game-version", required=True)
    pack.add_argument("--native-sha256", required=True)
    pack.add_argument("--build-id", required=True)
    pack.add_argument("--profile-sha256", required=True)
    pack.add_argument("--map", dest="map_name", required=True)
    pack.add_argument("--car", dest="car_name", required=True)
    pack.add_argument("--control-mode", choices=("manual", "touchdrive", "unknown"),
                      required=True)
    pack.add_argument("--target-tick", default="end")
    pack.add_argument("--notes", default="")

    inspect = sub.add_parser("inspect")
    inspect.add_argument("archive", type=Path)
    rename = sub.add_parser("rename")
    rename.add_argument("archive", type=Path)
    rename.add_argument("title")
    target = sub.add_parser("set-target")
    target.add_argument("archive", type=Path)
    target.add_argument("target_tick")
    unpack = sub.add_parser("unpack")
    unpack.add_argument("archive", type=Path)
    unpack.add_argument("output", type=Path)
    listing = sub.add_parser("list")
    listing.add_argument("library", type=Path)
    delete = sub.add_parser("delete")
    delete.add_argument("library", type=Path)
    delete.add_argument("archive", type=Path)
    delete.add_argument("--yes", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "pack":
            result = command_pack(args)
        elif args.command == "inspect":
            result = _summary(args.archive)
        elif args.command == "rename":
            result = command_rename(args)
        elif args.command == "set-target":
            result = command_set_target(args)
        elif args.command == "unpack":
            result = command_unpack(args)
        elif args.command == "list":
            result = command_list(args)
        elif args.command == "delete":
            result = command_delete(args)
        else:
            raise AssertionError(args.command)
    except (RecordingError, OSError) as error:
        print(json.dumps({"passed": False, "error": str(error)},
                         ensure_ascii=False), file=sys.stderr)
        return 1
    print(json.dumps({"passed": True, **result}, ensure_ascii=False,
                     indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
