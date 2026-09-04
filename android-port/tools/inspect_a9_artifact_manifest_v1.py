#!/usr/bin/env python3
"""Explicit batch manifest inspector (P2).

The user lists main-file/companion pairs explicitly in a JSON manifest.  This
tool validates each entry by calling the existing
``inspect_a9_artifact_v1.inspect_bytes`` (in-process, no subprocess), records
entry identity hashes and aggregates all results.

It is NOT a directory scanner and NEVER auto-discovers companions or files:
only paths written in the manifest are touched, and relative paths resolve
against the manifest's own directory.  Nothing is written; output goes to
stdout only.

Exit code = highest severity across entries: usage/io=3, unsupported=2,
invalid=1, all valid=0.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import inspect_a9_artifact_v1 as inspector

EXIT_OK = 0
EXIT_INVALID = 1
EXIT_UNSUPPORTED = 2
EXIT_IO_OR_USAGE = 3

SCHEMA_NAME = "A9_ARTIFACT_MANIFEST_V1"
MAX_ENTRIES = 256
ID_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")

ALLOWED_ENTRY_KEYS = ("id", "path", "companion", "format", "pid", "base")


@dataclass(frozen=True)
class ManifestEntry:
    id: str
    path: Path
    companion: Path | None
    format: str | None
    pid: int | None
    base: int | None


class ManifestError(ValueError):
    pass


def _parse_manifest(data: Any, manifest_dir: Path) -> list[ManifestEntry]:
    if not isinstance(data, dict):
        raise ManifestError("manifest root must be a JSON object")
    unknown_root = set(data) - {"schema", "entries"}
    if unknown_root:
        raise ManifestError(f"unknown top-level keys: {sorted(unknown_root)}")
    if data.get("schema") != SCHEMA_NAME:
        raise ManifestError(
            f"schema must be exactly {SCHEMA_NAME!r}, got {data.get('schema')!r}"
        )
    entries_raw = data.get("entries")
    if not isinstance(entries_raw, list):
        raise ManifestError("entries must be a list")
    if not 1 <= len(entries_raw) <= MAX_ENTRIES:
        raise ManifestError(
            f"entries count must be in 1..{MAX_ENTRIES}, got {len(entries_raw)}"
        )

    seen_ids: set[str] = set()
    entries: list[ManifestEntry] = []
    for index, item in enumerate(entries_raw):
        if not isinstance(item, dict):
            raise ManifestError(f"entry {index}: must be a JSON object")
        unknown = set(item) - set(ALLOWED_ENTRY_KEYS)
        if unknown:
            raise ManifestError(f"entry {index}: unknown keys {sorted(unknown)}")
        entry_id = item.get("id")
        if not isinstance(entry_id, str) or not ID_RE.fullmatch(entry_id):
            raise ManifestError(
                f"entry {index}: id must be 1..64 chars of [A-Za-z0-9_.-]"
            )
        if entry_id in seen_ids:
            raise ManifestError(f"duplicate entry id {entry_id!r}")
        seen_ids.add(entry_id)

        raw_path = item.get("path")
        if not isinstance(raw_path, str) or not raw_path.strip():
            raise ManifestError(f"entry {index} ({entry_id}): path must be a non-empty string")
        if "\x00" in raw_path:
            raise ManifestError(f"entry {index} ({entry_id}): path contains a NUL character")
        if any(char in raw_path for char in "*?[]"):
            raise ManifestError(
                f"entry {index} ({entry_id}): glob characters are not allowed in path"
            )
        path = Path(raw_path)
        if not path.is_absolute():
            path = manifest_dir / path

        raw_companion = None
        if "companion" in item:
            raw_companion = item["companion"]
            if not isinstance(raw_companion, str) or not raw_companion.strip():
                raise ManifestError(
                    f"entry {index} ({entry_id}): companion must be a non-empty "
                    "string when present"
                )
            if "\x00" in raw_companion:
                raise ManifestError(
                    f"entry {index} ({entry_id}): companion contains a NUL character"
                )
            if any(char in raw_companion for char in "*?[]"):
                raise ManifestError(
                    f"entry {index} ({entry_id}): glob characters are not allowed "
                    "in companion"
                )
        companion: Path | None = None
        if raw_companion:
            companion = Path(raw_companion)
            if not companion.is_absolute():
                companion = manifest_dir / companion

        format_name = item.get("format")
        if format_name is not None and format_name != "FC1":
            raise ManifestError(
                f"entry {index} ({entry_id}): format/pid/base are only allowed for FC1"
            )
        pid = item.get("pid")
        base = item.get("base")
        if format_name == "FC1":
            if companion is not None:
                raise ManifestError(
                    f"entry {index} ({entry_id}): FC1 entries cannot carry a companion"
                )
            if (isinstance(pid, bool) or not isinstance(pid, int)
                    or not 1 <= pid <= 0xFFFFFFFF):
                raise ManifestError(
                    f"entry {index} ({entry_id}): FC1 pid must be a strict int in "
                    "1..0xFFFFFFFF"
                )
            if base is None:
                raise ManifestError(f"entry {index} ({entry_id}): FC1 base is required")
            if isinstance(base, str):
                try:
                    base = int(base, 0)
                except ValueError as error:
                    raise ManifestError(
                        f"entry {index} ({entry_id}): FC1 base is not a valid integer"
                    ) from error
            if (isinstance(base, bool) or not isinstance(base, int)
                    or not 1 <= base <= 0xFFFFFFFFFFFFFFFF):
                raise ManifestError(
                    f"entry {index} ({entry_id}): FC1 base must be a strict int in "
                    "1..0xFFFFFFFFFFFFFFFF"
                )
        else:
            if format_name is not None or pid is not None or base is not None:
                raise ManifestError(
                    f"entry {index} ({entry_id}): format/pid/base are only allowed for FC1"
                )

        entries.append(
            ManifestEntry(
                id=entry_id,
                path=path,
                companion=companion,
                format=format_name,
                pid=pid,
                base=base,
            )
        )
    return entries


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _empty_report() -> dict[str, Any]:
    return {
        "status": None,
        "format": None,
        "version": None,
        "size": None,
        "frames": None,
        "first_tick": None,
        "last_tick": None,
        "checks": [],
        "validation_scope": "STRUCTURE_ONLY",
        "companion_status": "NOT_APPLICABLE",
        "companion_format": None,
        "capture_validated": False,
        "read_only": True,
        "device_access": 0,
        "error": None,
    }


def inspect_entry(entry: ManifestEntry) -> dict[str, Any]:
    """Inspect one manifest entry.  Returns an entry report (never writes)."""
    try:
        blob = entry.path.read_bytes()
    except OSError as error:
        report = _empty_report()
        report["status"] = "IO_ERROR"
        report["error"] = f"cannot read main file: {error}"
        report["size"] = 0
        report["main_sha256"] = None
        report["companion_sha256"] = None
        return report
    main_sha256 = _sha256(blob)
    companion_sha256: str | None = None

    # Main file first; companion path is only touched for an eligible,
    # structurally valid A9USR1-A9USR4 main file (same order as the CLI).
    report = inspector.inspect_bytes(
        blob, format_name=entry.format, pid=entry.pid, base=entry.base
    )
    main_format = report["format"]

    if entry.companion is not None:
        if report["status"] == "VALID" and main_format in inspector.COMPANION_FORMATS:
            try:
                companion_blob = entry.companion.read_bytes()
            except OSError as error:
                io_report = _empty_report()
                io_report.update({
                    "status": "IO_ERROR",
                    "format": main_format,
                    "version": report["version"],
                    "size": report["size"],
                    "frames": report["frames"],
                    "first_tick": report["first_tick"],
                    "last_tick": report["last_tick"],
                    "companion_status": "IO_ERROR",
                    "companion_format": "A9UTK1",
                    "error": f"cannot read companion file: {error}",
                })
                report = io_report
            else:
                companion_sha256 = _sha256(companion_blob)
                report = inspector.inspect_bytes(
                    blob, format_name=entry.format, pid=entry.pid, base=entry.base,
                    companion_blob=companion_blob,
                )
        elif report["status"] == "VALID":
            report = dict(report)
            report["status"] = "USAGE_ERROR"
            report["companion_status"] = "NOT_APPLICABLE"
            report["companion_format"] = "A9UTK1"
            report["error"] = (
                "companion is only supported for A9USR1-A9USR4 main files"
            )
        else:
            if main_format not in inspector.COMPANION_FORMATS:
                report = dict(report)
                report["status"] = "USAGE_ERROR"
                report["companion_status"] = "NOT_APPLICABLE"
                report["companion_format"] = "A9UTK1"
                report["error"] = (
                    "companion is only supported for A9USR1-A9USR4 main files"
                )
            else:
                report = dict(report)
                report["companion_status"] = "NOT_EVALUATED"
                report["companion_format"] = "A9UTK1"
                report["capture_validated"] = False

    report["main_sha256"] = main_sha256
    report["companion_sha256"] = companion_sha256
    return report


def _severity(report: dict[str, Any]) -> int:
    status = report.get("status")
    if status in ("IO_ERROR", "USAGE_ERROR"):
        return EXIT_IO_OR_USAGE
    if status == "UNSUPPORTED":
        return EXIT_UNSUPPORTED
    if status == "INVALID":
        return EXIT_INVALID
    return EXIT_OK


def run_manifest(manifest_path: Path) -> tuple[list[dict[str, Any]], dict[str, Any], str | None]:
    """Returns (entry_reports, summary, fatal_error).  Never writes."""
    try:
        manifest_blob = manifest_path.read_bytes()
    except OSError as error:
        return [], {"exit_code": EXIT_IO_OR_USAGE}, f"cannot read manifest: {error}"
    manifest_sha256 = _sha256(manifest_blob)
    try:
        data = json.loads(manifest_blob.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return [], {"exit_code": EXIT_IO_OR_USAGE}, f"manifest is not valid JSON: {error}"
    try:
        entries = _parse_manifest(data, manifest_path.resolve().parent)
    except ManifestError as error:
        return [], {"exit_code": EXIT_IO_OR_USAGE}, f"manifest schema error: {error}"

    entry_reports: list[dict[str, Any]] = []
    for entry in entries:
        item = inspect_entry(entry)
        item["id"] = entry.id
        item["path"] = str(entry.path)
        item["companion_path"] = str(entry.companion) if entry.companion else None
        entry_reports.append(item)

    counts = {"valid": 0, "invalid": 0, "unsupported": 0, "io_error": 0, "usage_error": 0}
    highest = EXIT_OK
    for item in entry_reports:
        status = item["status"]
        if status == "VALID":
            counts["valid"] += 1
        elif status == "INVALID":
            counts["invalid"] += 1
        elif status == "UNSUPPORTED":
            counts["unsupported"] += 1
        elif status == "IO_ERROR":
            counts["io_error"] += 1
        elif status == "USAGE_ERROR":
            counts["usage_error"] += 1
        highest = max(highest, _severity(item))

    summary = {
        "schema": SCHEMA_NAME,
        "manifest_sha256": manifest_sha256,
        "total": len(entry_reports),
        **counts,
        "exit_code": highest,
        "read_only": True,
        "device_access": 0,
        "auto_discovery": False,
    }
    return entry_reports, summary, None


def _render_text(entry_reports: list[dict[str, Any]], summary: dict[str, Any]) -> str:
    lines = [
        "A9_MANIFEST_SUMMARY "
        f"manifest_sha256={summary['manifest_sha256']} "
        f"entries={summary['total']} valid={summary['valid']} "
        f"invalid={summary['invalid']} unsupported={summary['unsupported']} "
        f"io_error={summary['io_error']} usage_error={summary['usage_error']} "
        f"read_only=1 device_access=0 auto_discovery=0"
    ]
    for item in entry_reports:
        main_sha = item.get("main_sha256") or "na"
        companion_sha = item.get("companion_sha256") or "na"
        lines.append(
            f"entry id={item['id']} path={item['path']} format={item['format'] or 'na'} "
            f"status={item['status']} validation_scope={item['validation_scope']} "
            f"companion_status={item['companion_status']} "
            f"capture_validated={'true' if item['capture_validated'] else 'false'} "
            f"main_sha256={main_sha} companion_sha256={companion_sha}"
        )
        if item.get("error"):
            lines.append(f"  error: {item['error']}")
    return "\n".join(lines)


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:  # pragma: no cover - argparse path
        self.print_usage(sys.stderr)
        print(f"manifest_error: {message}", file=sys.stderr)
        raise SystemExit(EXIT_IO_OR_USAGE)


def main(argv: list[str] | None = None) -> int:
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    entry_reports, summary, fatal = run_manifest(args.manifest)
    if fatal is not None:
        exit_code = summary.get("exit_code", EXIT_IO_OR_USAGE)
        if args.json:
            print(json.dumps({
                "schema": SCHEMA_NAME,
                "status": "ERROR",
                "error": fatal,
                "summary": {"exit_code": exit_code},
                "read_only": True,
                "device_access": 0,
                "auto_discovery": False,
            }, ensure_ascii=False, indent=2))
        else:
            print(f"manifest_error: {fatal}", file=sys.stderr)
        return exit_code

    if args.json:
        payload = {
            "schema": SCHEMA_NAME,
            "manifest_sha256": summary["manifest_sha256"],
            "entries": entry_reports,
            "summary": {k: summary[k] for k in
                        ("total", "valid", "invalid", "unsupported",
                         "io_error", "usage_error", "exit_code")},
            "read_only": True,
            "device_access": 0,
            "auto_discovery": False,
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print(_render_text(entry_reports, summary))
    return summary["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
