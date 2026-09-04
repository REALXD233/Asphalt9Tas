#!/usr/bin/env python3
"""Read-only consistency check between the binary-format registry document
(``docs/A9_BINARY_FORMAT_REGISTRY_20260818.md``) and the inspector's
``ALL_FORMATS`` / ``FormatSpec`` table in ``tools/inspect_a9_artifact_v1.py``.

Purpose: prevent drift between the code-supported format table and the
Markdown registry (format count, names, magic, version, support status and
A9UTK1 companion declarations).  The checker never writes any file.

Exit codes:
  0  consistent
  1  drift/duplicate/missing/field mismatch
  3  unreadable file or CLI usage error
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import inspect_a9_artifact_v1 as inspector

EXIT_OK = 0
EXIT_DRIFT = 1
EXIT_IO_OR_USAGE = 3

DEFAULT_REGISTRY = TOOLS_DIR.parent / "docs" / "A9_BINARY_FORMAT_REGISTRY_20260818.md"

STATUS_MAP = {
    "supported": "SUPPORTED",
    "requires_explicit_format": "REQUIRES_EXPLICIT_FORMAT",
    "known_unsupported": "KNOWN_UNSUPPORTED",
}

ROW_RE = re.compile(r"^\|.*\|\s*$")

# Supported-format table: | name | magic | version | hdr/frame | authority | status | companion | tick |
SUPPORTED_ROW_FIELDS = 8
# KNOWN_UNSUPPORTED table: | name | magic | version | decoder | reason |
UNSUPPORTED_ROW_FIELDS = 5

PATH_TOKEN_RE = re.compile(r"`((?:tools|android-port)/[A-Za-z0-9_./\\\-]+\.(?:py|md))`")


def _magic_bytes(ascii_magic: str) -> bytes:
    """``A9NPS1\\0\\0`` -> b'A9NPS1\\x00\\x00' (registry uses \\0 escapes)."""
    return ascii_magic.encode("latin-1").replace(b"\\0", b"\x00")


def _cells(row: str) -> list[str]:
    return [cell.strip() for cell in row.strip().strip("|").split("|")]


def _backtick(text: str) -> str | None:
    match = re.search(r"`([^`]*)`", text)
    return match.group(1) if match else None


def _extract_authority_path(cell: str) -> str | None:
    """Extract the normalized ``tools/...`` path from an authority cell.

    Ignores backticks, parenthesized function names and line numbers.  Returns
    None when the cell carries no real path (placeholder semantics, e.g.
    ``unknown(...)``).
    """
    match = re.search(r"`?([A-Za-z0-9_./\\-]+\.py)`?", cell)
    if not match:
        return None
    return match.group(1).replace("\\", "/")


@dataclass(frozen=True)
class MdEntry:
    name: str
    magic_ascii: str | None
    version: int | None
    status: str | None  # SUPPORTED / REQUIRES_EXPLICIT_FORMAT / KNOWN_UNSUPPORTED
    companion: str
    params: str
    authoritative: str
    row: str = ""


@dataclass
class CheckResult:
    format_count: int = 0
    errors: list[str] = field(default_factory=list)

    def error(self, message: str) -> None:
        self.errors.append(message)

    @property
    def consistent(self) -> bool:
        return not self.errors


def _parse_supported_rows(lines: list[str], result: CheckResult) -> list[MdEntry]:
    entries: list[MdEntry] = []
    for line in lines:
        if not ROW_RE.match(line):
            continue
        cells = _cells(line)
        if len(cells) != SUPPORTED_ROW_FIELDS:
            continue
        name = cells[0]
        if not name or not re.fullmatch(r"[A-Z0-9]+", name):
            continue
        magic = _backtick(cells[1])
        version = _parse_version(cells[2], name, result)
        status = _backtick(cells[5]) or cells[5]
        entries.append(
            MdEntry(
                name=name,
                magic_ascii=magic,
                version=version,
                status=status,
                companion=cells[6],
                params=cells[6],
                authoritative=cells[4],
                row=line,
            )
        )
    return entries


def _parse_unsupported_rows(lines: list[str], result: CheckResult) -> list[MdEntry]:
    entries: list[MdEntry] = []
    for line in lines:
        if not ROW_RE.match(line):
            continue
        cells = _cells(line)
        if len(cells) != UNSUPPORTED_ROW_FIELDS:
            continue
        name = cells[0]
        if not name or not re.fullmatch(r"[A-Z0-9]+", name):
            continue
        magic = _backtick(cells[1])
        version = _parse_version(cells[2], name, result)
        entries.append(
            MdEntry(
                name=name,
                magic_ascii=magic,
                version=version,
                status="KNOWN_UNSUPPORTED",
                companion="",
                params="",
                authoritative=cells[3],
                row=line,
            )
        )
    return entries


def _parse_version(text: str, name: str, result: CheckResult) -> int | None:
    """Parse a version cell: an integer, or ``na`` for a missing version."""
    text = text.strip()
    if text in ("", "na", "unknown"):
        return None
    try:
        return int(text)
    except ValueError:
        result.error(f"registry row {name}: non-integer version {text!r}")
        return None


def _parse_registry(text: str) -> tuple[list[MdEntry], CheckResult]:
    result = CheckResult()
    supported: list[MdEntry] = []
    unsupported: list[MdEntry] = []
    section: str | None = None
    for line in text.splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if section == "支持的格式":
            supported.extend(_parse_supported_rows([line], result))
        elif section is not None and section.startswith("KNOWN_UNSUPPORTED"):
            unsupported.extend(_parse_unsupported_rows([line], result))
    return supported + unsupported, result


def _collect_paths(text: str) -> list[str]:
    return [match.group(1) for match in PATH_TOKEN_RE.finditer(text)]


def check_registry(registry_path: Path) -> CheckResult:
    try:
        text = registry_path.read_text(encoding="utf-8")
    except OSError as error:
        result = CheckResult()
        result.error(f"cannot read registry: {error}")
        return result

    md_entries, result = _parse_registry(text)
    code_entries = {spec.name: spec for spec in inspector.ALL_FORMATS}

    md_names = [entry.name for entry in md_entries]
    if len(md_names) != len(set(md_names)):
        duplicates = {name for name in md_names if md_names.count(name) > 1}
        result.error(f"duplicate registry rows: {sorted(duplicates)}")
    result.format_count = len(md_entries)

    # 1) Format count matches the code table.
    if len(md_entries) != len(inspector.ALL_FORMATS):
        result.error(
            f"registry format count {len(md_entries)} != code format count "
            f"{len(inspector.ALL_FORMATS)}"
        )

    # 2) Name set matches exactly.
    md_name_set = set(md_names)
    code_name_set = set(code_entries)
    if md_name_set != code_name_set:
        missing = sorted(code_name_set - md_name_set)
        extra = sorted(md_name_set - code_name_set)
        if missing:
            result.error(f"registry missing formats: {missing}")
        if extra:
            result.error(f"registry has unknown formats: {extra}")

    for entry in md_entries:
        spec = code_entries.get(entry.name)
        if spec is None:
            continue  # already reported as missing/extra

        # 3) Magic and version consistency (all formats, including
        #    KNOWN_UNSUPPORTED: integer version or explicit ``na``).
        if entry.magic_ascii is not None:
            if spec.magic is not None:
                if _magic_bytes(entry.magic_ascii) != spec.magic:
                    result.error(
                        f"{entry.name}: registry magic {entry.magic_ascii!r} != "
                        f"code magic {spec.magic!r}"
                    )
            elif entry.name != "FC1":
                result.error(f"{entry.name}: registry declares magic but code has none")
        else:
            result.error(f"{entry.name}: registry magic missing")

        if spec.version is None:
            if entry.version is not None:
                result.error(
                    f"{entry.name}: registry version must be 'na', got {entry.version}"
                )
        elif entry.version != spec.version:
            result.error(
                f"{entry.name}: registry version {entry.version} != code version {spec.version}"
            )

        # 4) Status mapping.
        expected_status = STATUS_MAP.get(spec.kind)
        if expected_status is None:
            result.error(f"{entry.name}: unknown code kind {spec.kind!r}")
        elif entry.status != expected_status:
            result.error(
                f"{entry.name}: registry status {entry.status!r} != expected {expected_status!r}"
            )

        # 5/6) A9UTK1 companion declarations: only A9USR1-A9USR4 may carry one,
        # and each of them must declare the optional explicit companion.
        declares_companion = "A9UTK1" in entry.companion
        if entry.name in inspector.COMPANION_FORMATS:
            if not declares_companion:
                result.error(
                    f"{entry.name}: must declare the optional explicit A9UTK1 companion"
                )
        elif declares_companion:
            result.error(
                f"{entry.name}: only A9USR1-A9USR4 may declare an A9UTK1 companion"
            )

        # 7) FC1 explicit-format declaration and mandatory no-auto rule.
        if entry.name == "FC1":
            row_lower = entry.row.lower()
            if "--format fc1" not in row_lower or "--pid" not in row_lower or "--base" not in row_lower:
                result.error(
                    "FC1: registry must declare --format FC1 --pid --base requirements"
                )
            if "不用于自动识别" not in entry.row:
                result.error(
                    "FC1: registry must explicitly forbid automatic identification"
                )

        # 8) Authority is bound per-row: the normalized path extracted from
        #    the Markdown cell must equal FormatSpec.authoritative (or both be
        #    non-path placeholders).  File existence is checked separately.
        md_authority = _extract_authority_path(entry.authoritative)
        spec_authority = (
            spec.authoritative if spec.authoritative.startswith("tools/") else None
        )
        if md_authority != spec_authority:
            result.error(
                f"{entry.name}: registry authority {md_authority!r} != "
                f"code authority {spec_authority!r}"
            )

    # 8) Authoritative local paths exist (line numbers / parenthesized notes ignored).
    workspace_root = TOOLS_DIR.parent.parent
    seen_paths: set[str] = set()
    for raw in _collect_paths(text):
        normalized = raw.replace("/", "\\")
        if normalized in seen_paths:
            continue
        seen_paths.add(normalized)
        candidates: list[Path] = []
        if raw.startswith("android-port/"):
            candidates.append(workspace_root / normalized)
        else:
            # Registry paths are written relative to android-port/ (e.g.
            # ``tools/xxx.py``); also accept workspace-root-relative matches.
            candidates.append(workspace_root / "android-port" / normalized)
            candidates.append(workspace_root / normalized)
        if not any(candidate.exists() for candidate in candidates):
            result.error(f"registry references missing file: {raw}")

    return result


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:  # pragma: no cover - argparse path
        self.print_usage(sys.stderr)
        print(f"registry_check_error: {message}", file=sys.stderr)
        raise SystemExit(EXIT_IO_OR_USAGE)


def _render_text(result: CheckResult) -> str:
    lines = []
    for error in result.errors:
        lines.append(f"registry_error: {error}")
    if result.consistent:
        lines.append(
            f"REGISTRY_CONSISTENT format_count={result.format_count} "
            f"read_only=1 device_access=0"
        )
    else:
        lines.append(
            f"REGISTRY_INCONSISTENT format_count={result.format_count} "
            f"errors={len(result.errors)} read_only=1 device_access=0"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=None)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    registry_path = (args.registry or DEFAULT_REGISTRY).resolve()
    result = check_registry(registry_path)

    if result.errors and all("cannot read registry" in e for e in result.errors):
        if args.json:
            print(json.dumps({
                "consistent": False,
                "format_count": 0,
                "errors": result.errors,
                "read_only": True,
                "device_access": 0,
            }, ensure_ascii=False, indent=2))
        else:
            print(_render_text(result))
        return EXIT_IO_OR_USAGE

    if args.json:
        print(json.dumps({
            "consistent": result.consistent,
            "format_count": result.format_count,
            "errors": result.errors,
            "read_only": True,
            "device_access": 0,
        }, ensure_ascii=False, indent=2))
    else:
        print(_render_text(result))
    return EXIT_OK if result.consistent else EXIT_DRIFT


if __name__ == "__main__":
    raise SystemExit(main())
