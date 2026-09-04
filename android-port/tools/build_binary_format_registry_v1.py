#!/usr/bin/env python3
"""Build a read-only registry for the checked-in binary report formats.

The registry is derived from the offline parser scripts in `tools/`.
It extracts the declared format identity fields from each parser, records the
source path, and can optionally inspect a candidate file in read-only mode by
matching its leading magic/version bytes against the registry.
"""

from __future__ import annotations

import argparse
import ast
import dataclasses
import hashlib
import json
import os
import re
import struct
from pathlib import Path
from typing import Any


TOOLS_DIR = Path(__file__).resolve().parent
WORKSPACE_ROOT = TOOLS_DIR.parent.parent
EVIDENCE_DIR = WORKSPACE_ROOT / "android-port" / "evidence"

MAGIC_RE = re.compile(r"^MAGIC\s*=\s*(b(?:r)?['\"])(.+?)['\"]", re.M)
VERSION_RE = re.compile(r"^VERSION\s*=\s*(\d+)", re.M)
HEADER_SIZE_RE = re.compile(r"^HEADER_SIZE\s*=\s*(\d+)", re.M)
FRAME_SIZE_RE = re.compile(r"^FRAME_SIZE\s*=\s*(\d+)", re.M)
SNAPSHOT_SIZE_RE = re.compile(r"^SNAPSHOT_SIZE\s*=\s*(\d+)", re.M)
BUILD_ID_RE = re.compile(r"^(?:SUPPORTED_BUILD_ID|BUILD_ID)\s*=\s*bytes\.fromhex\(\"([0-9a-fA-F]+)\"\)", re.M)
HEADER_STRUCT_RE = re.compile(r"^HEADER\s*=\s*struct\.Struct\((.+)\)", re.M)
FRAME_STRUCT_RE = re.compile(r"^FRAME(?:_PREFIX)?\s*=\s*struct\.Struct\((.+)\)", re.M)
DOCSTRING_RE = re.compile(r'\A\s*"""(.*?)"""', re.S)


@dataclasses.dataclass(frozen=True)
class FormatEntry:
    parser: str
    path: str
    label: str
    magic_hex: str | None
    magic_ascii: str | None
    version: int | None
    header_size: int | None
    frame_size: int | None
    snapshot_size: int | None
    build_id: str | None
    struct_header: str | None
    struct_frame: str | None
    doc_summary: str | None


@dataclasses.dataclass(frozen=True)
class Registry:
    generated_at: str
    workspace_root: str
    entries: list[FormatEntry]


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _extract_doc_summary(text: str) -> str | None:
    match = DOCSTRING_RE.match(text)
    if not match:
        return None
    first_line = match.group(1).strip().splitlines()[0].strip()
    return first_line if first_line else None


def _extract_magic(text: str) -> tuple[str | None, str | None]:
    match = MAGIC_RE.search(text)
    if not match:
        return None, None
    literal = (match.group(1) + match.group(2) + "'") if match.group(1).endswith("'") else (match.group(1) + match.group(2) + '"')
    try:
        value = ast.literal_eval(literal)
    except Exception:
        return None, None
    if isinstance(value, (bytes, bytearray)):
        magic = bytes(value)
        ascii_repr = ''.join(chr(b) if 32 <= b < 127 else f"\\x{b:02x}" for b in magic)
        return magic.hex(), ascii_repr
    return None, None


def _extract_int(regex: re.Pattern[str], text: str) -> int | None:
    match = regex.search(text)
    if not match:
        return None
    return int(match.group(1))


def _extract_build_id(text: str) -> str | None:
    match = BUILD_ID_RE.search(text)
    return match.group(1).lower() if match else None


def _extract_struct(regex: re.Pattern[str], text: str) -> str | None:
    match = regex.search(text)
    if not match:
        return None
    return match.group(1).strip()


def _label_from_path(path: Path) -> str:
    name = path.stem
    name = name.replace("parse_", "")
    name = name.replace("_v1", "")
    name = name.replace("_v2", "")
    name = name.replace("_v3", "")
    name = name.replace("_v4", "")
    name = name.replace("_v5", "")
    name = name.replace("_v6", "")
    name = name.replace("_v7", "")
    return name


def _parser_entries() -> list[FormatEntry]:
    entries: list[FormatEntry] = []
    for path in sorted(TOOLS_DIR.glob("parse_*.py")):
        text = _read_text(path)
        magic_hex, magic_ascii = _extract_magic(text)
        version = _extract_int(VERSION_RE, text)
        header_size = _extract_int(HEADER_SIZE_RE, text)
        frame_size = _extract_int(FRAME_SIZE_RE, text)
        snapshot_size = _extract_int(SNAPSHOT_SIZE_RE, text)
        build_id = _extract_build_id(text)
        struct_header = _extract_struct(HEADER_STRUCT_RE, text)
        struct_frame = _extract_struct(FRAME_STRUCT_RE, text)
        entries.append(
            FormatEntry(
                parser=path.name,
                path=str(path.relative_to(WORKSPACE_ROOT)),
                label=_label_from_path(path),
                magic_hex=magic_hex,
                magic_ascii=magic_ascii,
                version=version,
                header_size=header_size,
                frame_size=frame_size,
                snapshot_size=snapshot_size,
                build_id=build_id,
                struct_header=struct_header,
                struct_frame=struct_frame,
                doc_summary=_extract_doc_summary(text),
            )
        )
    return entries


def build_registry() -> Registry:
    return Registry(
        generated_at=os.environ.get("BINARY_FORMAT_REGISTRY_GENERATED_AT", "2026-08-18T00:00:00Z"),
        workspace_root=str(WORKSPACE_ROOT),
        entries=_parser_entries(),
    )


def _read_prefix(path: Path, size: int = 64) -> bytes:
    with path.open("rb") as handle:
        return handle.read(size)


def _match_entry(entry: FormatEntry, blob: bytes) -> tuple[bool, list[str]]:
    reasons: list[str] = []
    if entry.magic_hex is not None:
        magic = bytes.fromhex(entry.magic_hex)
        if not blob.startswith(magic):
            reasons.append("magic mismatch")
    if entry.version is not None and len(blob) >= 12:
        version = struct.unpack_from("<I", blob, 8)[0]
        if version != entry.version:
            reasons.append("version mismatch")
    return not reasons, reasons


def inspect_file(path: Path, registry: Registry) -> dict[str, Any]:
    blob = _read_prefix(path, 64)
    candidates: list[dict[str, Any]] = []
    for entry in registry.entries:
        matched, reasons = _match_entry(entry, blob)
        if matched:
            candidates.append({"entry": dataclasses.asdict(entry), "matched": True, "reasons": []})
        elif entry.magic_hex is not None and blob.startswith(bytes.fromhex(entry.magic_hex[:16])):
            candidates.append({"entry": dataclasses.asdict(entry), "matched": False, "reasons": reasons})
    return {
        "path": str(path),
        "size": path.stat().st_size,
        "prefix_hex": blob.hex(),
        "candidates": candidates,
    }


def _render_markdown(registry: Registry) -> str:
    lines: list[str] = []
    lines.append("# Binary format registry 2026-08-18")
    lines.append("")
    lines.append(f"Workspace root `{registry.workspace_root}`")
    lines.append(f"Generated at {registry.generated_at}")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(f"Registered parsers: {len(registry.entries)}")
    lines.append("")
    lines.append("## Registry")
    lines.append("")
    lines.append("| Parser | Label | Magic | Version | Header | Frame | Snapshot | Build ID |")
    lines.append("|---|---|---|---:|---:|---:|---:|---|")
    for entry in registry.entries:
        lines.append(
            f"| `{entry.parser}` | {entry.label} | {entry.magic_ascii or ''} | {entry.version if entry.version is not None else ''} | "
            f"{entry.header_size if entry.header_size is not None else ''} | {entry.frame_size if entry.frame_size is not None else ''} | "
            f"{entry.snapshot_size if entry.snapshot_size is not None else ''} | {entry.build_id or ''} |"
        )
    lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="build the registry outputs")
    parser.add_argument("--inspect", type=Path, help="inspect a candidate binary/report file")
    parser.add_argument("--markdown-out", type=Path, default=None)
    parser.add_argument("--json-out", type=Path, default=None)
    args = parser.parse_args()

    registry = build_registry()
    md_path = args.markdown_out or (EVIDENCE_DIR / "BINARY_FORMAT_REGISTRY_20260818.md")
    json_path = args.json_out or (EVIDENCE_DIR / "BINARY_FORMAT_REGISTRY_20260818.json")

    if args.build or (not args.build and args.inspect is None):
        md_path.write_text(_render_markdown(registry), encoding="utf-8")
        json_path.write_text(json.dumps(dataclasses.asdict(registry), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"BINARY_FORMAT_REGISTRY_BUILT entries={len(registry.entries)} device_access=0")
        print(f"markdown_out={md_path}")
        print(f"json_out={json_path}")

    if args.inspect is not None:
        report = inspect_file(args.inspect, registry)
        matched = [c for c in report["candidates"] if c["matched"]]
        print(
            f"BINARY_FORMAT_INSPECT file={report['path']} size={report['size']} "
            f"matches={len(matched)} prefix_hex={report['prefix_hex'][:32]}..."
        )
        for candidate in matched[:5]:
            entry = candidate["entry"]
            print(
                f"match parser={entry['parser']} label={entry['label']} magic={entry['magic_ascii']} "
                f"version={entry['version']} header={entry['header_size']} frame={entry['frame_size']}"
            )
        if not matched:
            print("match=none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
