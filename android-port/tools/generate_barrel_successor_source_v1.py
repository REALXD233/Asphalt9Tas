#!/usr/bin/env python3
"""Generate a fail-closed Barrel successor source overlay.

The live-proven executor and its composition wrappers are immutable inputs.
This tool verifies their exact byte hashes, applies a complete allowlisted set
of exact byte replacements to an in-memory executor copy, copies the three
composition wrappers byte-for-byte, and publishes a new overlay directory in
one rename.  It never writes below ``android-port/src``.

No default replacement plan is provided deliberately.  A caller must supply a
complete reviewed JSON plan covering every required event-loop anchor.  This
prevents an install-only or otherwise partial successor from accidentally being
presented as a complete replay executor.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from dataclasses import dataclass
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Any, Iterable


PLAN_SCHEMA = "a9tas-barrel-successor-replacements-v1"
MANIFEST_SCHEMA = "a9tas-barrel-successor-generation-manifest-v1"

EXECUTOR_PATH = "src/hwbp_unified_tick_executor_v1.cpp"
WRAPPER_PATHS = (
    "src/hwbp_final_writer_unified_replay_v1.cpp",
    "src/hwbp_lifecycle_final_writer_replay_v1.cpp",
    "src/hwbp_lifecycle_final_writer_natural_action_replay_v1.cpp",
)
INTEGRATION_PATH = "src/final_writer_unified_integration_v1.h"

PINNED_SHA256 = {
    EXECUTOR_PATH:
        "dd58a7dc875f6502ad9eeffb737bc817f3be16ee915b345f5e67fb1188710e2f",
    WRAPPER_PATHS[0]:
        "9569cea7d0b38e155fe7b31f6cdf33d29060df87651bc30ed2f2aa3b29e17abd",
    WRAPPER_PATHS[1]:
        "7363c142314dff2616c89d9d55278069b254505c49858711eec0849a14c524c4",
    WRAPPER_PATHS[2]:
        "42ef782bb98cde7eb8b1ebfcaf58a78a99974a944e73c09d60cd8e75028980a6",
    INTEGRATION_PATH:
        "d5133430da068b27a28ed93a7718e568bcdde62b8767fc398a9617a1491ba593",
}

# Ordered application and manifest order.  Every plan must contain exactly
# these anchors.  The exact old/new bytes remain review inputs, not defaults.
REQUIRED_ANCHOR_IDS = (
    "include_barrel_integration",
    "runtime_capability_mask",
    "barrel_setup",
    "barrel_runtime_state",
    "c9c_barrel_layout",
    "post_phase_dr0",
    "f64_barrel_terminal",
    "barrel_deferred_release",
    "world_barrel_commit",
    "barrel_cleanup",
    "barrel_success_condition",
)

ENTRY_NAME = "barrel_successor_entry_v1.cpp"
MANIFEST_NAME = "generation-manifest.json"
ENTRY_BYTES = (
    b"// Generated composition entry; canonical inputs remain immutable.\n"
    b"#define A9TAS_BARREL_SUCCESSOR_V1 1\n"
    b"#include \"hwbp_lifecycle_final_writer_natural_action_replay_v1.cpp\"\n"
)


class GenerationError(RuntimeError):
    """A fail-closed validation or publication error."""


@dataclass(frozen=True)
class Replacement:
    anchor_id: str
    old: bytes
    new: bytes


@dataclass(frozen=True)
class ReplacementPlan:
    replacements: tuple[Replacement, ...]
    canonical_bytes: bytes
    canonical_sha256: str


@dataclass(frozen=True)
class AnchorRecord:
    anchor_id: str
    original_offset: int
    old_sha256: str
    new_sha256: str


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _require_plain_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise GenerationError(f"{field} must be a non-empty string")
    if "\x00" in value:
        raise GenerationError(f"{field} contains a NUL byte")
    return value


def _replacement_marker(anchor_id: str, edge: str) -> bytes:
    return f"// A9TAS_GENERATED_{edge} anchor={anchor_id}".encode("ascii")


def _json_object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise GenerationError(f"replacement plan contains duplicate JSON key: {key}")
        result[key] = value
    return result


def load_replacement_plan(path: Path) -> ReplacementPlan:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise GenerationError(f"cannot read replacement plan: {path}: {error}") from error
    try:
        document = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_json_object_without_duplicate_keys
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GenerationError(f"replacement plan is not valid UTF-8 JSON: {error}") from error
    if not isinstance(document, dict):
        raise GenerationError("replacement plan root must be an object")
    root_keys = {
        "schema", "complete_successor", "required_anchor_ids", "replacements"
    }
    if set(document) != root_keys:
        raise GenerationError(
            f"replacement plan root keys must be exactly {sorted(root_keys)}"
        )
    if document.get("schema") != PLAN_SCHEMA:
        raise GenerationError("replacement plan schema mismatch")
    if document.get("complete_successor") is not True:
        raise GenerationError("replacement plan is not marked complete_successor=true")
    declared = document.get("required_anchor_ids")
    if declared != list(REQUIRED_ANCHOR_IDS):
        raise GenerationError("replacement plan required_anchor_ids mismatch")
    raw_replacements = document.get("replacements")
    if not isinstance(raw_replacements, list):
        raise GenerationError("replacement plan replacements must be an array")

    parsed: dict[str, Replacement] = {}
    for index, item in enumerate(raw_replacements):
        if not isinstance(item, dict):
            raise GenerationError(f"replacements[{index}] must be an object")
        allowed_keys = {"id", "old_utf8", "new_utf8"}
        if set(item) != allowed_keys:
            raise GenerationError(
                f"replacements[{index}] keys must be exactly {sorted(allowed_keys)}"
            )
        anchor_id = _require_plain_string(item.get("id"), f"replacements[{index}].id")
        if anchor_id not in REQUIRED_ANCHOR_IDS:
            raise GenerationError(f"unknown replacement anchor: {anchor_id}")
        if anchor_id in parsed:
            raise GenerationError(f"duplicate replacement anchor: {anchor_id}")
        old_text = _require_plain_string(
            item.get("old_utf8"), f"replacements[{index}].old_utf8"
        )
        new_text = _require_plain_string(
            item.get("new_utf8"), f"replacements[{index}].new_utf8"
        )
        old = old_text.encode("utf-8")
        new = new_text.encode("utf-8")
        if old == new:
            raise GenerationError(f"replacement {anchor_id} does not change bytes")
        begin = _replacement_marker(anchor_id, "BEGIN")
        end = _replacement_marker(anchor_id, "END")
        if new.count(begin) != 1 or new.count(end) != 1:
            raise GenerationError(
                f"replacement {anchor_id} must contain exactly one generated BEGIN/END marker"
            )
        if new.find(begin) >= new.find(end):
            raise GenerationError(f"replacement {anchor_id} markers are out of order")
        parsed[anchor_id] = Replacement(anchor_id, old, new)

    missing = [anchor for anchor in REQUIRED_ANCHOR_IDS if anchor not in parsed]
    if missing or len(parsed) != len(REQUIRED_ANCHOR_IDS):
        raise GenerationError(f"replacement plan is incomplete; missing={missing}")
    ordered = tuple(parsed[anchor] for anchor in REQUIRED_ANCHOR_IDS)
    canonical_document = {
        "complete_successor": True,
        "replacements": [
            {
                "id": item.anchor_id,
                "new_utf8": item.new.decode("utf-8"),
                "old_utf8": item.old.decode("utf-8"),
            }
            for item in ordered
        ],
        "required_anchor_ids": list(REQUIRED_ANCHOR_IDS),
        "schema": PLAN_SCHEMA,
    }
    canonical = (
        json.dumps(
            canonical_document,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )
    return ReplacementPlan(ordered, canonical, sha256_bytes(canonical))


def verify_pinned_inputs(android_port_root: Path) -> dict[str, bytes]:
    root = android_port_root.resolve()
    if not root.is_dir():
        raise GenerationError(f"android-port root is not a directory: {root}")
    result: dict[str, bytes] = {}
    for relative, expected in PINNED_SHA256.items():
        candidate = (root / relative).resolve()
        try:
            candidate.relative_to(root)
        except ValueError as error:
            raise GenerationError(f"pinned input escapes android-port: {relative}") from error
        if not candidate.is_file():
            raise GenerationError(f"missing pinned input: {relative}")
        try:
            data = candidate.read_bytes()
        except OSError as error:
            raise GenerationError(f"cannot read pinned input {relative}: {error}") from error
        actual = sha256_bytes(data)
        if actual != expected:
            raise GenerationError(
                f"pinned input hash drift: {relative} expected={expected} actual={actual}"
            )
        result[relative] = data
    return result


def apply_replacements(
    source: bytes, plan: ReplacementPlan
) -> tuple[bytes, tuple[AnchorRecord, ...]]:
    plan_ids = tuple(item.anchor_id for item in plan.replacements)
    if plan_ids != REQUIRED_ANCHOR_IDS:
        raise GenerationError("in-memory replacement plan is not complete and ordered")
    spans: list[tuple[int, int, Replacement]] = []
    for replacement in plan.replacements:
        count = source.count(replacement.old)
        if count != 1:
            raise GenerationError(
                f"anchor {replacement.anchor_id} occurrence count is {count}, expected 1"
            )
        start = source.find(replacement.old)
        spans.append((start, start + len(replacement.old), replacement))
    ordered_spans = sorted(spans, key=lambda item: item[0])
    for previous, current in zip(ordered_spans, ordered_spans[1:]):
        if previous[1] > current[0]:
            raise GenerationError(
                f"replacement anchors overlap: {previous[2].anchor_id} and "
                f"{current[2].anchor_id}"
            )

    # Apply from the end of the original byte string so every recorded offset
    # remains an offset into the pinned source, independent of replacement size.
    derived = source
    for start, end, replacement in reversed(ordered_spans):
        derived = derived[:start] + replacement.new + derived[end:]
    records_by_id = {
        replacement.anchor_id: AnchorRecord(
            replacement.anchor_id,
            start,
            sha256_bytes(replacement.old),
            sha256_bytes(replacement.new),
        )
        for start, _end, replacement in ordered_spans
    }
    records = tuple(records_by_id[anchor] for anchor in REQUIRED_ANCHOR_IDS)
    for anchor in REQUIRED_ANCHOR_IDS:
        if derived.count(_replacement_marker(anchor, "BEGIN")) != 1 or derived.count(
            _replacement_marker(anchor, "END")
        ) != 1:
            raise GenerationError(f"generated marker cardinality failed: {anchor}")
    return derived, records


def _canonical_json_bytes(document: dict[str, Any]) -> bytes:
    return (
        json.dumps(document, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8")
        + b"\n"
    )


def _generator_sha256() -> str:
    try:
        return sha256_bytes(Path(__file__).resolve().read_bytes())
    except OSError as error:
        raise GenerationError(f"cannot hash generator source: {error}") from error


def prepare_outputs(
    android_port_root: Path, plan: ReplacementPlan
) -> tuple[dict[str, bytes], dict[str, Any]]:
    inputs = verify_pinned_inputs(android_port_root)
    derived, anchor_records = apply_replacements(inputs[EXECUTOR_PATH], plan)
    output_files: dict[str, bytes] = {
        Path(EXECUTOR_PATH).name: derived,
        ENTRY_NAME: ENTRY_BYTES,
    }
    for relative in WRAPPER_PATHS:
        output_files[Path(relative).name] = inputs[relative]

    manifest = {
        "anchor_records": [
            {
                "id": record.anchor_id,
                "new_sha256": record.new_sha256,
                "old_sha256": record.old_sha256,
                "original_offset": record.original_offset,
            }
            for record in anchor_records
        ],
        "baseline_unchanged": True,
        "complete_successor": True,
        "generator_sha256": _generator_sha256(),
        "input_files": [
            {"path": relative, "sha256": PINNED_SHA256[relative]}
            for relative in PINNED_SHA256
        ],
        "output_files": [
            {"path": name, "sha256": sha256_bytes(output_files[name])}
            for name in sorted(output_files)
        ],
        "replacement_plan_sha256": plan.canonical_sha256,
        "required_anchor_ids": list(REQUIRED_ANCHOR_IDS),
        "schema": MANIFEST_SCHEMA,
    }
    output_files[MANIFEST_NAME] = _canonical_json_bytes(manifest)
    return output_files, manifest


def _write_new_file(path: Path, data: bytes) -> None:
    with path.open("xb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def _validate_output_scope(android_port_root: Path, output_dir: Path) -> Path:
    root = android_port_root.resolve()
    source = (root / "src").resolve()
    build = (root / "build").resolve()
    output = output_dir.resolve(strict=False)
    if output == root or output == source or source in output.parents:
        raise GenerationError("output directory may not be android-port or below src")
    if output in root.parents:
        raise GenerationError("output directory may not contain android-port")
    if root in output.parents and build not in output.parents:
        raise GenerationError("output below android-port must be inside build")
    return output


def publish_outputs(
    android_port_root: Path,
    output_dir: Path,
    output_files: dict[str, bytes],
) -> None:
    output = _validate_output_scope(android_port_root, output_dir)
    parent = output.parent
    parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise GenerationError(f"output directory already exists: {output}")
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-", dir=parent))
    published = False
    try:
        for name in sorted(output_files):
            if Path(name).name != name:
                raise GenerationError(f"non-local output name rejected: {name}")
            _write_new_file(staging / name, output_files[name])

        # Close the read/publish time-of-check gap.  Mutation of any canonical
        # input while staging causes the whole unpublished directory to vanish.
        verify_pinned_inputs(android_port_root)
        if output.exists():
            raise GenerationError(f"output directory appeared during generation: {output}")
        os.replace(staging, output)
        published = True
    except OSError as error:
        raise GenerationError(f"atomic output publication failed: {error}") from error
    finally:
        if not published and staging.exists():
            shutil.rmtree(staging)


def generate(
    android_port_root: Path,
    replacement_plan_path: Path,
    output_dir: Path | None,
    *,
    check_only: bool = False,
) -> dict[str, Any]:
    plan = load_replacement_plan(replacement_plan_path)
    output_files, manifest = prepare_outputs(android_port_root, plan)
    if check_only:
        return manifest
    if output_dir is None:
        raise GenerationError("--output-dir is required unless --check-only is used")
    publish_outputs(android_port_root, output_dir, output_files)
    return manifest


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--android-port-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--replacements-json", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check-only", action="store_true")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _argument_parser().parse_args(list(argv) if argv is not None else None)
    try:
        manifest = generate(
            args.android_port_root,
            args.replacements_json,
            args.output_dir,
            check_only=args.check_only,
        )
    except GenerationError as error:
        print(f"BARREL_SUCCESSOR_SOURCE_GENERATOR passed=0 error={error}", file=sys.stderr)
        return 1
    derived = next(
        item["sha256"]
        for item in manifest["output_files"]
        if item["path"] == Path(EXECUTOR_PATH).name
    )
    print(
        "BARREL_SUCCESSOR_SOURCE_GENERATOR passed=1 "
        f"check_only={int(args.check_only)} anchors={len(REQUIRED_ANCHOR_IDS)} "
        f"derived_sha256={derived} baseline_unchanged=1 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
