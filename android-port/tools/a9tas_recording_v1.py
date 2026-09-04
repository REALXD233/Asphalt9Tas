#!/usr/bin/env python3
"""Canonical G9 recording container and strict A9G4R2 decoder.

The outer A9TAS1 container adds product metadata and integrity without
re-encoding the live-proven A9G4R2 payload.  Unpacking therefore preserves
every recorded float bit and receipt byte exactly.
"""

from __future__ import annotations

import dataclasses
import hashlib
import hmac
import json
import math
import re
import struct
import uuid
from datetime import datetime
from pathlib import Path
from typing import Any


MAGIC = b"A9TAS1\0\0"
VERSION = 1
FLAGS = 0x7  # canonical manifest, embedded A9G4R2, raw-bits authoritative
HEADER = struct.Struct("<8sIIIIQIIIIQII32s32s32s")
HEADER_SIZE = HEADER.size
MAX_MANIFEST_SIZE = 64 * 1024
MAX_RECORDING_SIZE = 512 * 1024 * 1024
TARGET_END = 0xFFFFFFFF

A9G4R2_MAGIC = b"A9G4R2\0\0"
A9G4R2_HEADER = struct.Struct("<8sIIIIIIIIQII8s")
A9G4R2_FRAME = struct.Struct("<QQfffIIB3s3f2f64s12sII")
A9G4R2_INTERVAL = struct.Struct("<QII")
A9G4R2_LEGACY_VERSION = 2
A9G4R2_VERSION = 3
A9G4R2_LEGACY_FLAGS = 0x0F
A9G4R2_FLAGS = 0x1F
A9G4R2_LEGACY_SKIP = 0x78
A9G4R2_SKIP = 0x48

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
BUILD_ID_RE = re.compile(r"^[0-9a-f]{40}$")
PACKAGE_RE = re.compile(r"^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+$")
CONTROL_MODES = frozenset({"manual", "touchdrive", "unknown"})


class RecordingError(ValueError):
    """A stable validation error suitable for UI error mapping."""


@dataclasses.dataclass(frozen=True)
class A9G4R2Summary:
    version: int
    frame_count: int
    interval_count: int
    fixed_delta_us: int
    session_id: int
    generation: int
    nitro_calls: int
    barrel_nonzero_frames: int


@dataclasses.dataclass(frozen=True)
class A9TasArchive:
    manifest: dict[str, Any]
    recording: bytes
    source: A9G4R2Summary
    recording_sha256: str


def _finite_raw_floats(raw: bytes, count: int) -> bool:
    values = struct.unpack(f"<{count}f", raw)
    return all(math.isfinite(value) and abs(value) <= 1_000_000.0
               for value in values)


def decode_a9g4r2(data: bytes) -> A9G4R2Summary:
    """Strictly validate the embedded live recording and return its identity."""
    if len(data) < A9G4R2_HEADER.size:
        raise RecordingError("a9g4r2.short_header")
    fields = A9G4R2_HEADER.unpack_from(data)
    (magic, version, header_size, frame_size, interval_size, frame_count,
     interval_count, fixed_delta_us, flags, session_id, generation,
     reserved0, reserved) = fields
    legacy = version == A9G4R2_LEGACY_VERSION and flags == A9G4R2_LEGACY_FLAGS
    current = version == A9G4R2_VERSION and flags == A9G4R2_FLAGS
    if (magic != A9G4R2_MAGIC or not (legacy or current) or
            header_size != A9G4R2_HEADER.size or
            frame_size != A9G4R2_FRAME.size or
            interval_size != A9G4R2_INTERVAL.size or
            frame_count == 0 or interval_count == 0 or
            not 1_000 <= fixed_delta_us <= 100_000 or
            session_id == 0 or generation == 0 or reserved0 != 0 or
            reserved != bytes(8)):
        raise RecordingError("a9g4r2.header_identity")
    expected_size = (A9G4R2_HEADER.size + frame_count * A9G4R2_FRAME.size +
                     interval_count * A9G4R2_INTERVAL.size)
    if expected_size != len(data):
        raise RecordingError("a9g4r2.exact_size")

    offset = A9G4R2_HEADER.size
    nitro_calls = 0
    barrel_frames = 0
    for index in range(frame_count):
        frame = A9G4R2_FRAME.unpack_from(data, offset)
        offset += A9G4R2_FRAME.size
        (tick, monotonic_ns, steering, brake, accelerator, nitro, skip,
         respawn, padding, ax, ay, az, rbx0, rbx1, transform, linear,
         frame_flags, frame_reserved) = frame
        if (tick != index or monotonic_ns != index * fixed_delta_us * 1_000 or
                not math.isfinite(steering) or abs(steering) > 1.0 or
                not math.isfinite(brake) or abs(brake) > 1.05 or
                accelerator != 0.0 or nitro > 2 or
                skip != (A9G4R2_LEGACY_SKIP if legacy else A9G4R2_SKIP) or
                respawn != 0 or padding != bytes(3) or
                (legacy and any(value != 0.0
                                for value in (ax, ay, az, rbx0, rbx1))) or
                (current and any(not math.isfinite(value) or
                                 abs(value) > 1_000_000.0
                                 for value in (ax, ay, az, rbx0, rbx1))) or
                not _finite_raw_floats(transform, 16) or
                not _finite_raw_floats(linear, 3) or
                frame_flags != 0x7 or frame_reserved != 0):
            raise RecordingError(f"a9g4r2.frame.{index}")
        nitro_calls += nitro
        barrel_frames += int(any(value != 0.0
                                 for value in (ax, ay, az, rbx0, rbx1)))

    last_tick = -1
    next_ordinal = 0
    calls_per_tick = [0] * frame_count
    for index in range(interval_count):
        tick, ordinal, output_bits = A9G4R2_INTERVAL.unpack_from(data, offset)
        offset += A9G4R2_INTERVAL.size
        value = struct.unpack("<f", struct.pack("<I", output_bits))[0]
        if (tick >= frame_count or not math.isfinite(value) or
                not 0.001 <= value <= 0.1):
            raise RecordingError(f"a9g4r2.interval_value.{index}")
        if tick != last_tick:
            if tick <= last_tick:
                raise RecordingError(f"a9g4r2.interval_order.{index}")
            last_tick = tick
            next_ordinal = 0
        if ordinal != next_ordinal:
            raise RecordingError(f"a9g4r2.interval_ordinal.{index}")
        next_ordinal += 1
        calls_per_tick[tick] += 1
    if any(count == 0 for count in calls_per_tick):
        raise RecordingError("a9g4r2.missing_tick_interval")
    return A9G4R2Summary(
        version=version,
        frame_count=frame_count,
        interval_count=interval_count,
        fixed_delta_us=fixed_delta_us,
        session_id=session_id,
        generation=generation,
        nitro_calls=nitro_calls,
        barrel_nonzero_frames=barrel_frames,
    )


def canonical_manifest_bytes(manifest: dict[str, Any]) -> bytes:
    return json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _bounded_text(value: Any, field: str, maximum: int,
                  *, nonempty: bool = True) -> str:
    if not isinstance(value, str) or (nonempty and not value) or len(value) > maximum:
        raise RecordingError(f"manifest.{field}")
    if any(ord(character) < 0x20 for character in value):
        raise RecordingError(f"manifest.{field}.control_character")
    return value


def _validate_manifest(manifest: dict[str, Any], source: A9G4R2Summary,
                       target_tick: int) -> None:
    if not isinstance(manifest, dict) or set(manifest) != {
        "schema", "recording_id", "title", "created_utc", "game", "race",
        "recording", "notes",
    }:
        raise RecordingError("manifest.keys")
    if manifest["schema"] != "a9tas.recording.v1":
        raise RecordingError("manifest.schema")
    try:
        parsed_id = uuid.UUID(_bounded_text(manifest["recording_id"],
                                            "recording_id", 36))
    except (ValueError, AttributeError) as error:
        raise RecordingError("manifest.recording_id") from error
    if str(parsed_id) != manifest["recording_id"]:
        raise RecordingError("manifest.recording_id.canonical")
    _bounded_text(manifest["title"], "title", 120)
    created = _bounded_text(manifest["created_utc"], "created_utc", 32)
    try:
        stamp = datetime.fromisoformat(created.replace("Z", "+00:00"))
    except ValueError as error:
        raise RecordingError("manifest.created_utc") from error
    if not created.endswith("Z") or stamp.tzinfo is None:
        raise RecordingError("manifest.created_utc.utc")
    _bounded_text(manifest["notes"], "notes", 2_000, nonempty=False)

    game = manifest["game"]
    if not isinstance(game, dict) or set(game) != {
        "package", "version", "native_sha256", "build_id",
        "build_profile_sha256",
    }:
        raise RecordingError("manifest.game.keys")
    package = _bounded_text(game["package"], "game.package", 191)
    if PACKAGE_RE.fullmatch(package) is None:
        raise RecordingError("manifest.game.package")
    _bounded_text(game["version"], "game.version", 64)
    if (not isinstance(game["native_sha256"], str) or
            SHA256_RE.fullmatch(game["native_sha256"]) is None):
        raise RecordingError("manifest.game.native_sha256")
    if (not isinstance(game["build_profile_sha256"], str) or
            SHA256_RE.fullmatch(game["build_profile_sha256"]) is None):
        raise RecordingError("manifest.game.build_profile_sha256")
    if (not isinstance(game["build_id"], str) or
            BUILD_ID_RE.fullmatch(game["build_id"]) is None):
        raise RecordingError("manifest.game.build_id")

    race = manifest["race"]
    if not isinstance(race, dict) or set(race) != {"map", "car", "control_mode"}:
        raise RecordingError("manifest.race.keys")
    _bounded_text(race["map"], "race.map", 120)
    _bounded_text(race["car"], "race.car", 120)
    if race["control_mode"] not in CONTROL_MODES:
        raise RecordingError("manifest.race.control_mode")

    recording = manifest["recording"]
    if (not isinstance(recording, dict) or set(recording) != {
            "format", "format_version", "frame_count", "interval_count",
            "fixed_delta_us", "session_id", "generation", "target_tick",
            } or any(type(recording[field]) is not int for field in (
                "format_version", "frame_count", "interval_count",
                "fixed_delta_us", "session_id", "generation", "target_tick"))):
        raise RecordingError("manifest.recording_types")
    expected_recording = {
        "format": "A9G4R2",
        "format_version": source.version,
        "frame_count": source.frame_count,
        "interval_count": source.interval_count,
        "fixed_delta_us": source.fixed_delta_us,
        "session_id": source.session_id,
        "generation": source.generation,
        "target_tick": target_tick,
    }
    if recording != expected_recording:
        raise RecordingError("manifest.recording_identity")


def build_manifest(*, source: A9G4R2Summary, recording_id: str, title: str,
                   created_utc: str, game_package: str, game_version: str,
                   native_sha256: str, build_id: str,
                   build_profile_sha256: str, map_name: str, car_name: str,
                   control_mode: str, target_tick: int, notes: str = "") -> dict[str, Any]:
    manifest = {
        "schema": "a9tas.recording.v1",
        "recording_id": recording_id,
        "title": title,
        "created_utc": created_utc,
        "game": {
            "package": game_package,
            "version": game_version,
            "native_sha256": native_sha256.lower(),
            "build_id": build_id.lower(),
            "build_profile_sha256": build_profile_sha256.lower(),
        },
        "race": {"map": map_name, "car": car_name,
                 "control_mode": control_mode},
        "recording": {
            "format": "A9G4R2",
            "format_version": source.version,
            "frame_count": source.frame_count,
            "interval_count": source.interval_count,
            "fixed_delta_us": source.fixed_delta_us,
            "session_id": source.session_id,
            "generation": source.generation,
            "target_tick": target_tick,
        },
        "notes": notes,
    }
    _validate_manifest(manifest, source, target_tick)
    return manifest


def encode_archive(manifest: dict[str, Any], recording: bytes) -> bytes:
    source = decode_a9g4r2(recording)
    target_tick = manifest.get("recording", {}).get("target_tick")
    if (not isinstance(target_tick, int) or isinstance(target_tick, bool) or
            not 0 <= target_tick < source.frame_count):
        raise RecordingError("target_tick")
    _validate_manifest(manifest, source, target_tick)
    manifest_bytes = canonical_manifest_bytes(manifest)
    if len(manifest_bytes) > MAX_MANIFEST_SIZE:
        raise RecordingError("manifest.too_large")
    if len(recording) > MAX_RECORDING_SIZE:
        raise RecordingError("recording.too_large")
    manifest_hash = hashlib.sha256(manifest_bytes).digest()
    recording_hash = hashlib.sha256(recording).digest()
    header = HEADER.pack(
        MAGIC, VERSION, HEADER_SIZE, FLAGS, len(manifest_bytes), len(recording),
        source.frame_count, source.interval_count, source.fixed_delta_us,
        source.version, source.session_id, source.generation, target_tick,
        manifest_hash, recording_hash, bytes(32),
    )
    return header + manifest_bytes + recording


def decode_archive(data: bytes) -> A9TasArchive:
    if len(data) < HEADER_SIZE:
        raise RecordingError("archive.short_header")
    fields = HEADER.unpack_from(data)
    (magic, version, header_size, flags, manifest_size, recording_size,
     frame_count, interval_count, fixed_delta_us, source_version, session_id,
     generation, target_tick, manifest_hash, recording_hash, reserved) = fields
    if (magic != MAGIC or version != VERSION or header_size != HEADER_SIZE or
            flags != FLAGS or not 1 <= manifest_size <= MAX_MANIFEST_SIZE or
            not 1 <= recording_size <= MAX_RECORDING_SIZE or
            reserved != bytes(32)):
        raise RecordingError("archive.header_identity")
    if len(data) != HEADER_SIZE + manifest_size + recording_size:
        raise RecordingError("archive.exact_size")
    manifest_bytes = data[HEADER_SIZE:HEADER_SIZE + manifest_size]
    recording = data[HEADER_SIZE + manifest_size:]
    if not hmac.compare_digest(hashlib.sha256(manifest_bytes).digest(),
                               manifest_hash):
        raise RecordingError("archive.manifest_hash")
    if not hmac.compare_digest(hashlib.sha256(recording).digest(),
                               recording_hash):
        raise RecordingError("archive.recording_hash")
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RecordingError("archive.manifest_json") from error
    if canonical_manifest_bytes(manifest) != manifest_bytes:
        raise RecordingError("archive.manifest_not_canonical")
    source = decode_a9g4r2(recording)
    if (frame_count != source.frame_count or
            interval_count != source.interval_count or
            fixed_delta_us != source.fixed_delta_us or
            source_version != source.version or session_id != source.session_id or
            generation != source.generation or target_tick >= source.frame_count):
        raise RecordingError("archive.source_identity")
    _validate_manifest(manifest, source, target_tick)
    return A9TasArchive(
        manifest=manifest,
        recording=recording,
        source=source,
        recording_sha256=recording_hash.hex(),
    )


def read_archive(path: Path) -> A9TasArchive:
    try:
        return decode_archive(path.read_bytes())
    except OSError as error:
        raise RecordingError(f"archive.io.{error}") from error
