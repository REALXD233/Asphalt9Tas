#!/usr/bin/env python3
"""Build a fail-closed evidence index for android-port evidence documents.

Classification is driven primarily by the document title and an explicit
``Verdict:`` / ``## Verdict`` / ``## Result`` line.  Full-text scanning is
used only as a secondary fallback when the structured fields are absent.

The index also extracts inline SHA-256 values, checks referenced local files
that exist in the workspace, and binds declared SHA-256 values to the files
they appear next to.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


STATUS_VALUES = {
    "LIVE_PASS",
    "OFFLINE_PASS",
    "BUILD_ONLY",
    "FAIL_CLOSED",
    "RETIRED",
    "SUPERSEDED",
    "UNKNOWN",
}

SHA256_RE = re.compile(r"\b([a-fA-F0-9]{64})\b")
DATE_RE = re.compile(r"\b20\d{2}-\d{2}-\d{2}\b")
TITLE_RE = re.compile(r"^#\s+(.*)$", re.MULTILINE)
VERDICT_RE = re.compile(
    r"^\s*(?:\*\*)?(?:Verdict|Result|Status)(?:\*\*)?\s*[:：]\s*(.+)$",
    re.MULTILINE | re.IGNORECASE,
)
VERDICT_HEADER_RE = re.compile(
    r"^#+\s*(?:Verdict|Result|Status)\s*$",
    re.MULTILINE | re.IGNORECASE,
)

# Tight path grammar: only backtick-delimited or clearly quoted paths.
BACKTICK_PATH_RE = re.compile(r"`([^`\n]+)`")
# An explicit file-path token with a file extension (``src/foo.cpp``,
# ``tools/parse_conditional_audit_v1.py``, ``build/a.ps1`` ...).
PATH_TOKEN_RE = re.compile(r"[A-Za-z0-9_./\\\-]+\.[A-Za-z0-9]+")
# <SHA-256> <path> on the same line.  ``[ \t]+`` deliberately does not cross
# newlines: a hash and a path on different lines are only paired when they are
# literally adjacent lines (see _extract_declared_hashes).  Optional backticks
# around either side are tolerated.
HASH_PATH_SAME_LINE_RE = re.compile(
    r"(`?)([a-fA-F0-9]{64})(`?)[ \t]+(`?)([A-Za-z0-9_./\\\-]+\.[A-Za-z0-9]+)(`?)"
)
# <path> <SHA-256> on the same line.
PATH_HASH_SAME_LINE_RE = re.compile(
    r"(`?)([A-Za-z0-9_./\\\-]+\.[A-Za-z0-9]+)(`?)[ \t]+(`?)([a-fA-F0-9]{64})(`?)"
)
# A line that is exactly one 64-hex value (optionally backticked, trailing
# punctuation allowed) — used for ``<path>`` / ``<hash>`` adjacent-line pairs.
HASH_ONLY_LINE_RE = re.compile(r"^[ \t]*`?([a-fA-F0-9]{64})`?[ \t]*[.,;:]?[ \t]*$")
# Source reference with a line/line-range suffix, e.g.
# ``ref-alu-tas-v2/DetourFunctions.cpp:1435-1464``.
LINE_RANGE_RE = re.compile(r"^(.*\.[A-Za-z0-9]+):\d+(?:-\d+)?$")

# Words that mark a backticked candidate as a whole command line rather than a
# local file reference (``python android-port/tools/...``, ``adb pull`` ...).
COMMAND_WORDS = frozenset(
    {"python", "python3", "py", "adb", "su", "sh", "bash", "pwsh",
     "powershell", "cmd", "pip"}
)
# Workspace-relative directory prefixes that make a slash-containing candidate
# a concrete local path even when it lacks a trailing extension.
PATH_PREFIXES = (
    "android-port/", "tools/", "src/", "evidence/", "ref-alu-tas-v2/",
    "source/", "build/", "toolchains/", "docs/", "ida/", "apk-analysis/",
)


@dataclass(frozen=True)
class ReferencedFileCheck:
    reference: str
    resolved_path: str | None
    exists: bool
    sha256: str | None = None
    declared_sha256: str | None = None
    hash_matches: bool | None = None
    status: str = "UNKNOWN"
    note: str | None = None


@dataclass(frozen=True)
class DeclaredHash:
    sha256: str
    referenced_path: str


@dataclass(frozen=True)
class EvidenceRecord:
    file: str
    date: str | None
    title: str | None
    subject: str | None
    declared_result: str | None
    verdict_text: str | None
    status: str
    status_reason: str
    status_source: str
    sha256_values: list[str] = field(default_factory=list)
    declared_hashes: list[DeclaredHash] = field(default_factory=list)
    referenced_files: list[ReferencedFileCheck] = field(default_factory=list)
    conflicts: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class EvidenceIndex:
    generated_at: str
    workspace_root: str
    evidence_root: str
    counts: dict[str, int]
    records: list[EvidenceRecord]
    conflicts: list[dict[str, str]]
    missing_references: list[dict[str, str]]
    ambiguous_references: list[dict[str, str]] = field(default_factory=list)
    hash_mismatches: list[dict[str, str]] = field(default_factory=list)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _normalize_status_token(raw: str) -> str | None:
    if not raw:
        return None
    upper = raw.strip().upper()
    # Normalize common punctuation variants.
    upper = upper.replace("-", "_").replace(" ", "_").replace("/", "_")
    # Collapse repeated underscores.
    upper = re.sub(r"_+", "_", upper)
    if upper in STATUS_VALUES:
        return upper
    # Accept loose forms like "BUILD ONLY PASS" -> BUILD_ONLY
    if "BUILD" in upper and "ONLY" in upper:
        return "BUILD_ONLY"
    if "OFFLINE" in upper and "PASS" in upper:
        return "OFFLINE_PASS"
    if "LIVE" in upper and "PASS" in upper:
        return "LIVE_PASS"
    if "RETIRED" in upper or "PERMANENTLY_RETIRED" in upper:
        return "RETIRED"
    if "SUPERSEDED" in upper or "SUPERSEDE" in upper:
        return "SUPERSEDED"
    if "FAIL" in upper and "CLOSED" in upper:
        return "FAIL_CLOSED"
    return None


def _extract_title(text: str) -> str | None:
    match = TITLE_RE.search(text)
    return match.group(1).strip() if match else None


def _extract_date(text: str, filename: str) -> str | None:
    match = DATE_RE.search(text)
    if match:
        return match.group(0)
    match = re.search(r"(20\d{6})", filename)
    if match:
        date = match.group(0)
        return f"{date[0:4]}-{date[4:6]}-{date[6:8]}"
    return None


def _split_title(title: str | None) -> tuple[str | None, str | None, str | None]:
    if not title:
        return None, None, None
    # Prefer em dash; fall back to hyphen with spaces.
    parts = re.split(r"\s+[—]\s+", title, maxsplit=1)
    if len(parts) == 1:
        parts = re.split(r"\s+[-]\s+", title, maxsplit=1)
    subject = parts[0].strip() if parts else title.strip()
    declared = parts[1].strip() if len(parts) > 1 else None
    return title, subject or None, declared or None


def _extract_verdict_text(text: str) -> str | None:
    for pattern in (VERDICT_RE, VERDICT_HEADER_RE):
        match = pattern.search(text)
        if match:
            # For header form, read the next non-empty line.
            if pattern is VERDICT_HEADER_RE:
                after = text[match.end():]
                lines = after.splitlines()
                for line in lines:
                    stripped = line.strip()
                    if stripped:
                        return stripped
            else:
                return match.group(1).strip()
    return None


def _classify_from_title(filename: str, title: str | None) -> tuple[str | None, str]:
    """Try to classify from the title and filename alone.

    ``live result`` is deliberately NOT a LIVE_PASS marker: it only proves an
    experiment happened.  Only explicit pass conclusions count.
    """
    lower_name = filename.lower()
    if "superseded" in lower_name:
        return "SUPERSEDED", "filename superseded marker"
    if "live_crash" in lower_name:
        return "RETIRED", "filename live_crash marker"

    if title:
        lower_title = title.lower()
        if "superseded" in lower_title:
            return "SUPERSEDED", "title superseded marker"
        if "permanently retired" in lower_title or "route retired" in lower_title:
            return "RETIRED", "title retirement marker"
        if "offline pass" in lower_title or "offline-pass" in lower_title:
            return "OFFLINE_PASS", "title offline-pass marker"
        if "build-only" in lower_title or "build only" in lower_title or "build/design-only" in lower_title:
            return "BUILD_ONLY", "title build-only marker"
        if "live pass" in lower_title or "live-pass" in lower_title or "实机通过" in lower_title:
            return "LIVE_PASS", "title live-pass marker"
        if "crash" in lower_title:
            return "RETIRED", "title crash marker"
        if ("fail closed" in lower_title or "fail-closed" in lower_title
                or "failed closed" in lower_title or "拒绝" in lower_title
                or "rejected" in lower_title):
            return "FAIL_CLOSED", "title fail-closed/rejected marker"
        if "retired" in lower_title or "退役" in lower_title:
            return "RETIRED", "title retirement marker"
    return None, "no decisive title marker"


def _classify_from_verdict(verdict_text: str | None) -> tuple[str | None, str]:
    """Classify from an explicit Verdict/Result/Status line.

    Fail-closed and rejected conclusions are checked before any bare PASS
    token so that a verdict like "preflight passed; install rejected ..."
    stays FAIL_CLOSED.  A bare ``PASS``/``passed``/``passes`` conclusion is a
    clear-pass marker whose live/offline kind is resolved by the surrounding
    document context in _classify_status.
    """
    if not verdict_text:
        return None, "no verdict line"
    lower = verdict_text.lower()
    if "superseded" in lower or "supersede" in lower:
        return "SUPERSEDED", "verdict superseded"
    if "permanently retired" in lower or "retired" in lower or "退役" in lower:
        return "RETIRED", "verdict retired"
    if ("fail closed" in lower or "fail-closed" in lower or "failed closed" in lower
            or "rejected" in lower or "rejection" in lower or "拒绝" in lower
            or "did not pass" in lower or "failed" in lower):
        return "FAIL_CLOSED", "verdict fail-closed/rejected"
    if "build-only" in lower or "build only" in lower or "build/design-only" in lower:
        return "BUILD_ONLY", "verdict build-only"
    if "offline pass" in lower or "offline-pass" in lower:
        return "OFFLINE_PASS", "verdict offline-pass"
    if "live pass" in lower or "live-pass" in lower or "passed live" in lower or "实机通过" in lower:
        return "LIVE_PASS", "verdict live-pass"
    if re.search(r"\b(pass|passed|passes)\b", lower):
        return "CLEAR_PASS", "verdict clear pass conclusion"
    return None, "verdict present but no status keyword"


def _classify_from_filename(filename: str) -> tuple[str | None, str]:
    lower_name = filename.lower()
    if "superseded" in lower_name:
        return "SUPERSEDED", "filename superseded marker"
    if "live_crash" in lower_name:
        return "RETIRED", "filename live_crash marker"
    if "build_only" in lower_name:
        return "BUILD_ONLY", "filename build_only marker"
    if "offline_pass" in lower_name:
        return "OFFLINE_PASS", "filename offline_pass marker"
    if "live_pass" in lower_name:
        return "LIVE_PASS", "filename live_pass marker"
    if re.search(r"(?:^|_)reject(?:_|$)", lower_name):
        return "FAIL_CLOSED", "filename reject marker"
    return None, "no decisive filename marker"


def _has_live_context(filename: str, title: str | None, verdict_text: str | None) -> bool:
    joined = " ".join(
        part.lower() for part in (filename, title or "", verdict_text or "")
    )
    if "live_pass" in filename.lower() or "live_result" in filename.lower():
        return True
    return any(
        marker in joined
        for marker in ("live result", "live pass", "live-pass", "实机", "passed live")
    )


def _has_offline_context(filename: str, title: str | None, verdict_text: str | None) -> bool:
    joined = " ".join(
        part.lower() for part in (filename, title or "", verdict_text or "")
    )
    return "offline_pass" in filename.lower() or "offline pass" in joined or "offline-pass" in joined


def _classify_status(
    filename: str, title: str | None, verdict_text: str | None
) -> tuple[str, str, str, str | None, str | None]:
    """Return (status, reason, source, title_status, verdict_status).

    An explicit Verdict/Result/Status conclusion wins over the plain title.
    A title that explicitly claims a different status is preserved as a
    conflict (see _status_conflict) while the verdict status is used.
    """
    title_status, title_reason = _classify_from_title(filename, title)
    verdict_status, verdict_reason = _classify_from_verdict(verdict_text)
    filename_status, filename_reason = _classify_from_filename(filename)

    if verdict_status == "CLEAR_PASS":
        if _has_live_context(filename, title, verdict_text):
            verdict_status = "LIVE_PASS"
            verdict_reason = "verdict clear pass conclusion (live context)"
        elif _has_offline_context(filename, title, verdict_text):
            verdict_status = "OFFLINE_PASS"
            verdict_reason = "verdict clear pass conclusion (offline context)"
        else:
            verdict_status = None

    if verdict_status is not None:
        return verdict_status, verdict_reason, "verdict", title_status, verdict_status

    if title_status is not None:
        return title_status, title_reason, "title", title_status, verdict_status

    if filename_status is not None:
        return filename_status, filename_reason, "filename", title_status, verdict_status

    return "UNKNOWN", "no decisive marker in title/verdict/filename", "none", title_status, verdict_status


def _extract_sha256_values(text: str) -> list[str]:
    values: list[str] = []
    seen: set[str] = set()
    for match in SHA256_RE.finditer(text):
        value = match.group(1).lower()
        if value not in seen:
            seen.add(value)
            values.append(value)
    return values


def _path_tokens_in_line(line: str) -> list[str]:
    """Return the explicit path tokens (extension required) in a single line."""
    return [m.group(0) for m in PATH_TOKEN_RE.finditer(line)]


def _extract_declared_hashes(text: str) -> list[DeclaredHash]:
    """Extract (sha256, path) pairs from explicit, adjacency-bounded formats.

    Supported formats (nothing else is bound):
    - ``<SHA-256> <path>`` on one line;
    - ``<path> <SHA-256>`` on one line;
    - adjacent-line alternating runs of bare hash lines and single path-token
      lines; the run's direction is taken from its first element, so both
      ``<path>`` / ``<hash>`` and ``<hash>`` / ``<path>`` manifest layouts are
      paired exactly as written (no shifted bindings).

    A hash that cannot be paired with an explicit path is left unbound.
    """
    results: list[DeclaredHash] = []
    seen: set[tuple[str, str]] = set()

    def add(sha: str, path: str) -> None:
        sha = sha.lower()
        path = path.strip().strip("`").strip()
        key = (sha, path)
        if key not in seen:
            seen.add(key)
            results.append(DeclaredHash(sha256=sha, referenced_path=path))

    for match in HASH_PATH_SAME_LINE_RE.finditer(text):
        add(match.group(2), match.group(5))
    for match in PATH_HASH_SAME_LINE_RE.finditer(text):
        add(match.group(5), match.group(2))

    lines = text.splitlines()

    def line_kind(line: str) -> tuple[str, str] | None:
        """Return ('hash', value) / ('path', token) or None for other lines."""
        hash_match = HASH_ONLY_LINE_RE.match(line)
        if hash_match and not _path_tokens_in_line(line):
            return "hash", hash_match.group(1)
        tokens = _path_tokens_in_line(line)
        if len(tokens) == 1 and not re.search(r"[a-fA-F0-9]{64}", line):
            return "path", tokens[0]
        return None

    index = 0
    while index < len(lines):
        kind = line_kind(lines[index])
        if kind is None:
            index += 1
            continue
        run: list[tuple[str, str]] = []
        cursor = index
        while cursor < len(lines):
            item = line_kind(lines[cursor])
            if item is None:
                break
            run.append(item)
            cursor += 1
        if len(run) >= 2 and run[0][0] == "hash":
            for offset in range(0, len(run) - 1, 2):
                if run[offset][0] == "hash" and run[offset + 1][0] == "path":
                    add(run[offset][1], run[offset + 1][1])
        elif len(run) >= 2 and run[0][0] == "path":
            for offset in range(0, len(run) - 1, 2):
                if run[offset][0] == "path" and run[offset + 1][0] == "hash":
                    add(run[offset + 1][1], run[offset][1])
        index = cursor
    return results


def _iter_backtick_paths(text: str) -> Iterable[str]:
    """Only extract backtick-delimited paths — the tightest signal."""
    for match in BACKTICK_PATH_RE.finditer(text):
        candidate = match.group(1).strip()
        if candidate:
            yield candidate


def _clean_candidate(candidate: str) -> str:
    """Strip quoting/wrapping punctuation from a raw backtick candidate."""
    cleaned = candidate.strip().strip("()[]{}<>")
    cleaned = cleaned.replace("`", "").strip('"').strip("'")
    cleaned = cleaned.rstrip(".,;:")
    return cleaned


def _strip_line_range(candidate: str) -> str:
    """``ref-alu-tas-v2/DetourFunctions.cpp:1435-1464`` -> the file path."""
    match = LINE_RANGE_RE.match(candidate)
    if match:
        return match.group(1)
    return candidate


def _is_path_shaped(candidate: str) -> bool:
    """True when the candidate is a concrete workspace-relative path shape.

    A slash-containing candidate counts as concrete when it ends in a file
    extension or starts with a known workspace directory prefix.
    """
    lower = candidate.lower()
    has_sep = bool(re.search(r"[\\/]", candidate))
    has_ext = bool(re.search(r"\.[a-z][a-z0-9]*$", lower))
    if has_sep and has_ext:
        return True
    if has_sep:
        return any(lower.startswith(prefix) for prefix in PATH_PREFIXES)
    return False


def _looks_like_file_reference(candidate: str) -> bool:
    """Conservative: reject ratios, offsets, device paths, commands, globs.

    Only candidates that could plausibly name a local file survive this gate.
    Unresolvable basenames are handled separately as ambiguous, never as
    missing workspace references.
    """
    if not candidate:
        return False
    lower = candidate.lower()
    if lower.startswith(("http://", "https://")):
        return False
    if re.fullmatch(r"0x[0-9a-f]+", lower) or lower.startswith("+0x"):
        return False
    if re.fullmatch(r"[0-9a-f]{64}", lower):
        return False
    # Address/offset expressions carry a 0x token (``module+0x38B7578``,
    # ``0x4D10E94/+0x4``, ``SIGSEGV/0xdead0000``, ``native+0x10..+0x4F``).
    if re.search(r"0x[0-9a-f]+", lower):
        return False
    # Globs.
    if "*" in candidate or "?" in candidate:
        return False
    # Android/device absolute paths are never workspace files.
    if candidate.startswith("/"):
        return False
    # Section/expression tokens like ``.data/.bss`` are not file references.
    if candidate.startswith("."):
        return False
    # Whole command lines (``python android-port/tools/x.py``, ``adb pull``).
    if " " in candidate:
        first = candidate.split(None, 1)[0].lower()
        if first in COMMAND_WORDS:
            return False
    # Test ratios ``7/7``, ``0/0/1`` and counter pairs ``activations=1/2``.
    if re.fullmatch(r"\d+(?:/\d+)+", candidate):
        return False
    if re.fullmatch(r"[+-]?[\d.]+/[+-]?[\d.]+", candidate):
        return False
    if re.fullmatch(r"[A-Za-z0-9_.-]+=\d+(?:/\d+)+", candidate):
        return False
    # Format-name/count pairs such as ``A9USR3/3``.
    if re.fullmatch(r"[A-Za-z0-9_-]+/\d+", candidate):
        return False
    # Register/sequence ranges such as ``x0..x7``, ``X0..X18``.
    if re.fullmatch(r"[A-Za-z0-9_]+\.\.[A-Za-z0-9_]+", candidate):
        return False
    if "SIGSEGV" in candidate:
        return False
    # Must have a path separator + extension, a known directory prefix, or be
    # a bare basename with an extension (resolvable in the allowed dirs).
    return _is_path_shaped(candidate) or bool(
        re.search(r"\.[a-z][a-z0-9]*$", lower)
    )


BASENAME_RESOLUTION_DIRS = (
    ".",
    "android-port",
    "android-port/tools",
    "android-port/src",
    "android-port/evidence",
    "ref-alu-tas-v2",
)


def _resolve_reference(workspace_root: Path, evidence_dir: Path, candidate: str) -> Path | None:
    cleaned = _clean_candidate(candidate)
    cleaned = _strip_line_range(cleaned)
    if not cleaned:
        return None

    direct = Path(cleaned)
    possibilities: list[Path] = []
    if direct.is_absolute():
        possibilities.append(direct)
    else:
        possibilities.extend([
            (workspace_root / direct),
            (evidence_dir / direct),
            (workspace_root / "android-port" / direct),
        ])
        if cleaned.startswith("android-port/"):
            possibilities.append(workspace_root / cleaned)
    for path in possibilities:
        try:
            if path.exists():
                return path.resolve()
        except OSError:
            continue

    # Bare basename: resolve uniquely inside the allowed directories.
    if "/" not in cleaned and "\\" not in cleaned:
        matches: list[Path] = []
        for relative in BASENAME_RESOLUTION_DIRS:
            path = workspace_root / relative / cleaned
            try:
                if path.is_file():
                    matches.append(path.resolve())
            except OSError:
                continue
        if len(matches) == 1:
            return matches[0]
        # Zero or multiple matches: ambiguous, not a confirmed workspace file.
    return None


def _check_referenced_files(
    workspace_root: Path,
    evidence_dir: Path,
    content: str,
    declared_hashes: list[DeclaredHash],
) -> tuple[list[ReferencedFileCheck], list[dict[str, str]]]:
    checks: list[ReferencedFileCheck] = []
    hash_mismatches: list[dict[str, str]] = []
    seen: set[str] = set()

    # Build a lookup of declared hash by normalized path.
    declared_by_path: dict[str, str] = {}
    for dh in declared_hashes:
        key = dh.referenced_path.lower().replace("\\", "/")
        declared_by_path[key] = dh.sha256

    # Collect candidates from backtick paths and declared hash lines.
    candidates: list[str] = []
    for candidate in _iter_backtick_paths(content):
        candidates.append(candidate)
    for dh in declared_hashes:
        candidates.append(dh.referenced_path)

    for candidate in candidates:
        cleaned = _strip_line_range(_clean_candidate(candidate))
        if not _looks_like_file_reference(cleaned):
            continue
        if candidate in seen:
            continue
        seen.add(candidate)
        resolved = _resolve_reference(workspace_root, evidence_dir, cleaned)
        declared = declared_by_path.get(cleaned.lower().replace("\\", "/"))

        if resolved is None:
            if _is_path_shaped(cleaned):
                checks.append(
                    ReferencedFileCheck(
                        reference=candidate,
                        resolved_path=None,
                        exists=False,
                        declared_sha256=declared,
                        status="MISSING",
                        note="not found in workspace",
                    )
                )
            else:
                checks.append(
                    ReferencedFileCheck(
                        reference=candidate,
                        resolved_path=None,
                        exists=False,
                        declared_sha256=declared,
                        status="AMBIGUOUS",
                        note="unresolvable basename; not a confirmed workspace file",
                    )
                )
            continue

        sha = None
        note = None
        try:
            if resolved.is_file():
                sha = _sha256(resolved)
            else:
                note = "resolved path is not a file"
        except OSError as error:
            note = f"sha256 failed: {error}"

        hash_matches = None
        if declared and sha:
            hash_matches = declared == sha
            if not hash_matches:
                hash_mismatches.append(
                    {
                        "file": "",
                        "reference": candidate,
                        "declared": declared,
                        "actual": sha,
                        "resolved_path": str(resolved),
                    }
                )

        checks.append(
            ReferencedFileCheck(
                reference=candidate,
                resolved_path=str(resolved),
                exists=resolved.exists(),
                sha256=sha,
                declared_sha256=declared,
                hash_matches=hash_matches,
                status="FOUND" if resolved.exists() else "MISSING",
                note=note,
            )
        )
    return checks, hash_mismatches


def _status_conflict(
    filename: str,
    declared: str | None,
    status: str,
    title_status: str | None,
    verdict_status: str | None,
    pre_status: str,
) -> list[str]:
    issues: list[str] = []
    if declared:
        norm_declared = _normalize_status_token(declared)
        if norm_declared is not None and norm_declared != status:
            issues.append(
                f"declared result suggests {norm_declared} but classifier chose {status}"
            )
    if (
        title_status is not None
        and verdict_status is not None
        and title_status != verdict_status
    ):
        issues.append(
            f"title suggests {title_status} but explicit verdict concludes {verdict_status}"
        )
    if filename.startswith("PT_NB0_") and pre_status != "RETIRED":
        issues.append(
            f"PT-NB0 must be RETIRED by policy (document classified as {pre_status})"
        )
    if filename.startswith("FC1_GUARDED_RUNNER_") and pre_status != "BUILD_ONLY":
        issues.append(
            f"FC-1 guarded runner must be BUILD_ONLY (document classified as {pre_status})"
        )
    return issues


def _render_markdown(index: EvidenceIndex) -> str:
    lines: list[str] = []
    lines.append("# Evidence index 2026-08-18")
    lines.append("")
    lines.append(f"Generated at {index.generated_at}")
    lines.append(f"Workspace root `{index.workspace_root}`")
    lines.append(f"Evidence root `{index.evidence_root}`")
    lines.append("")
    counts = index.counts
    lines.append("## Summary")
    lines.append("")
    lines.append(
        "| Status | Count |\n|---|---:|\n"
        + "\n".join(f"| {status} | {counts.get(status, 0)} |" for status in sorted(STATUS_VALUES))
    )
    lines.append("")
    lines.append(f"Total evidence files: {counts.get('TOTAL', 0)}")
    lines.append(f"Files with reference problems: {counts.get('MISSING_REFERENCES', 0)}")
    lines.append(f"Files with status conflicts: {counts.get('CONFLICTS', 0)}")
    lines.append(f"Bound declared hashes: {counts.get('BOUND_HASHES', 0)}")
    lines.append(f"Unbound SHA-256 values: {counts.get('UNBOUND_HASHES', 0)}")
    lines.append(f"Hash mismatches: {counts.get('HASH_MISMATCHES', 0)}")
    lines.append("")
    lines.append("## Evidence records")
    lines.append("")
    lines.append("| File | Date | Status | Source | Declared result | SHA-256 | Refs | Notes |")
    lines.append("|---|---|---|---|---|---:|---:|---|")
    for record in index.records:
        local_refs = sum(1 for ref in record.referenced_files if ref.exists)
        note = "; ".join(record.conflicts) if record.conflicts else record.status_reason
        lines.append(
            f"| `{record.file}` | {record.date or ''} | {record.status} | {record.status_source} | "
            f"{record.declared_result or ''} | {len(record.sha256_values)} | {local_refs} | "
            f"{note.replace('|', '\\|')} |"
        )
    lines.append("")
    if index.conflicts:
        lines.append("## Status conflicts")
        lines.append("")
        for item in index.conflicts:
            lines.append(f"- `{item['file']}` — {item['message']}")
        lines.append("")
    if index.hash_mismatches:
        lines.append("## Hash mismatches")
        lines.append("")
        for item in index.hash_mismatches:
            lines.append(
                f"- `{item['file']}`: `{item['reference']}` declared={item['declared']} "
                f"actual={item['actual']}"
            )
        lines.append("")
    if index.missing_references:
        lines.append("## Missing references")
        lines.append("")
        for item in index.missing_references:
            lines.append(
                f"- `{item['file']}` references `{item['reference']}`"
            )
        lines.append("")
    if index.ambiguous_references:
        lines.append("## Ambiguous references")
        lines.append("")
        lines.append(
            "Basenames that could not be confirmed as workspace files (device "
            "libraries, unverifiable labels); reported for transparency, not "
            "counted as missing."
        )
        lines.append("")
        for item in index.ambiguous_references:
            lines.append(
                f"- `{item['file']}` references `{item['reference']}`"
            )
        lines.append("")
    lines.append("## Per-file detail")
    lines.append("")
    for record in index.records:
        lines.append(f"### {record.file}")
        lines.append("")
        lines.append(f"- Date: {record.date or 'unknown'}")
        lines.append(f"- Title: {record.title or 'unknown'}")
        lines.append(f"- Subject: {record.subject or 'unknown'}")
        lines.append(f"- Declared result: {record.declared_result or 'unknown'}")
        lines.append(f"- Verdict text: {record.verdict_text or 'none'}")
        lines.append(f"- Status: {record.status}")
        lines.append(f"- Status source: {record.status_source}")
        lines.append(f"- Status reason: {record.status_reason}")
        lines.append(f"- Inline SHA-256 count: {len(record.sha256_values)}")
        bound_shas = {dh.sha256 for dh in record.declared_hashes}
        lines.append(f"- Bound declared hashes: {len(record.declared_hashes)}")
        lines.append(
            "- Unbound SHA-256 values: "
            f"{sum(1 for value in record.sha256_values if value not in bound_shas)}"
        )
        if record.declared_hashes:
            lines.append("- Declared hash bindings:")
            for dh in record.declared_hashes:
                lines.append(f"  - `{dh.sha256}` -> `{dh.referenced_path}`")
        if record.conflicts:
            lines.append("- Conflicts:")
            for conflict in record.conflicts:
                lines.append(f"  - {conflict}")
        if record.referenced_files:
            lines.append("- Local references:")
            for ref in record.referenced_files:
                parts = [f"`{ref.reference}`"]
                if ref.resolved_path:
                    parts.append(f"-> `{ref.resolved_path}`")
                parts.append(f"exists={'1' if ref.exists else '0'}")
                if ref.sha256:
                    parts.append(f"sha256={ref.sha256}")
                if ref.declared_sha256:
                    parts.append(f"declared={ref.declared_sha256}")
                if ref.hash_matches is not None:
                    parts.append(f"match={'1' if ref.hash_matches else '0'}")
                lines.append(f"  - {' '.join(parts)}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def _write_json(index: EvidenceIndex) -> str:
    return json.dumps(asdict(index), ensure_ascii=False, indent=2) + "\n"


GENERATED_ARTIFACTS = frozenset(
    {
        "EVIDENCE_INDEX_20260818.md",
        "OFFLINE_REGRESSION_SUITE_V1.md",
        "DOCUMENT_STALENESS_AUDIT_20260818.md",
        "HANDOFF_FOR_MAIN_AGENT_REVIEW_20260818.md",
    }
)


def build_index(workspace_root: Path) -> EvidenceIndex:
    evidence_root = workspace_root / "android-port" / "evidence"
    if not evidence_root.exists():
        raise FileNotFoundError(f"evidence directory not found: {evidence_root}")

    records: list[EvidenceRecord] = []
    conflicts: list[dict[str, str]] = []
    missing_refs: list[dict[str, str]] = []
    ambiguous_refs: list[dict[str, str]] = []
    hash_mismatches: list[dict[str, str]] = []
    status_counts = {status: 0 for status in STATUS_VALUES}
    status_counts.update(
        {
            "TOTAL": 0,
            "CONFLICTS": 0,
            "MISSING_REFERENCES": 0,
            "HASH_MISMATCHES": 0,
            "BOUND_HASHES": 0,
            "UNBOUND_HASHES": 0,
        }
    )

    for path in sorted(evidence_root.glob("*.md")):
        if path.name in GENERATED_ARTIFACTS:
            continue
        content = _read_text(path)
        title = _extract_title(content)
        subject, declared_result = None, None
        if title:
            _, subject, declared_result = _split_title(title)
        date = _extract_date(content, path.name)
        verdict_text = _extract_verdict_text(content)
        sha_values = _extract_sha256_values(content)
        declared_hashes = _extract_declared_hashes(content)
        status, status_reason, status_source, title_status, verdict_status = (
            _classify_status(path.name, title, verdict_text)
        )
        pre_status = status
        # Policy normalization happens BEFORE counting and record creation:
        # PT-NB0 is permanently retired; the FC-1 guarded runner is build-only.
        if path.name.startswith("PT_NB0_") and status != "RETIRED":
            status = "RETIRED"
            status_reason = "PT-NB0 route permanently retired by policy"
            status_source = "policy"
        elif path.name.startswith("FC1_GUARDED_RUNNER_") and status != "BUILD_ONLY":
            status = "BUILD_ONLY"
            status_reason = "FC-1 guarded runner build-only by policy"
            status_source = "policy"
        record_conflicts = _status_conflict(
            path.name, declared_result, status, title_status, verdict_status, pre_status
        )
        refs, file_hash_mismatches = _check_referenced_files(
            workspace_root, evidence_root, content, declared_hashes
        )
        for mismatch in file_hash_mismatches:
            mismatch["file"] = path.name
            hash_mismatches.append(mismatch)
        for ref in refs:
            if ref.status == "MISSING":
                missing_refs.append(
                    {
                        "file": path.name,
                        "reference": ref.reference,
                        "resolved_path": ref.resolved_path or "",
                        "declared_sha256": ref.declared_sha256 or "",
                    }
                )
            elif ref.status == "AMBIGUOUS":
                ambiguous_refs.append(
                    {
                        "file": path.name,
                        "reference": ref.reference,
                        "resolved_path": "",
                        "declared_sha256": ref.declared_sha256 or "",
                    }
                )
        for conflict in record_conflicts:
            conflicts.append({"file": path.name, "message": conflict})
        status_counts[status] = status_counts.get(status, 0) + 1
        status_counts["TOTAL"] += 1
        status_counts["CONFLICTS"] += len(record_conflicts)
        status_counts["MISSING_REFERENCES"] += sum(
            1 for ref in refs if ref.status == "MISSING"
        )
        status_counts["HASH_MISMATCHES"] += len(file_hash_mismatches)
        bound_shas: set[str] = set()
        for dh in declared_hashes:
            bound_shas.add(dh.sha256)
        status_counts["BOUND_HASHES"] += len(declared_hashes)
        status_counts["UNBOUND_HASHES"] += sum(
            1 for value in sha_values if value not in bound_shas
        )
        records.append(
            EvidenceRecord(
                file=path.name,
                date=date,
                title=title,
                subject=subject,
                declared_result=declared_result,
                verdict_text=verdict_text,
                status=status,
                status_reason=status_reason,
                status_source=status_source,
                sha256_values=sha_values,
                declared_hashes=declared_hashes,
                referenced_files=refs,
                conflicts=record_conflicts,
            )
        )

    generated_at = os.environ.get(
        "EVIDENCE_INDEX_GENERATED_AT", "2026-08-18T00:00:00Z"
    )
    return EvidenceIndex(
        generated_at=generated_at,
        workspace_root=str(workspace_root),
        evidence_root=str(evidence_root),
        counts=status_counts,
        records=records,
        conflicts=conflicts,
        missing_references=missing_refs,
        ambiguous_references=ambiguous_refs,
        hash_mismatches=hash_mismatches,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace-root", type=Path, default=None)
    parser.add_argument("--markdown-out", type=Path, default=None)
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--stdout-only", action="store_true")
    args = parser.parse_args(argv)

    script_dir = Path(__file__).resolve().parent
    default_workspace = script_dir.parent.parent
    workspace_root = (args.workspace_root or default_workspace).resolve()
    index = build_index(workspace_root)

    markdown_path = args.markdown_out or (
        workspace_root / "android-port" / "evidence" / "EVIDENCE_INDEX_20260818.md"
    )
    json_path = args.json_out or (
        workspace_root / "android-port" / "evidence" / "EVIDENCE_INDEX_20260818.json"
    )

    markdown_text = _render_markdown(index)
    json_text = _write_json(index)

    if not args.stdout_only:
        markdown_path.write_text(markdown_text, encoding="utf-8")
        json_path.write_text(json_text, encoding="utf-8")

    print(
        f"EVIDENCE_INDEX_SUMMARY total={index.counts.get('TOTAL', 0)} "
        f"live_pass={index.counts.get('LIVE_PASS', 0)} "
        f"offline_pass={index.counts.get('OFFLINE_PASS', 0)} "
        f"build_only={index.counts.get('BUILD_ONLY', 0)} "
        f"fail_closed={index.counts.get('FAIL_CLOSED', 0)} "
        f"retired={index.counts.get('RETIRED', 0)} "
        f"superseded={index.counts.get('SUPERSEDED', 0)} "
        f"unknown={index.counts.get('UNKNOWN', 0)} "
        f"conflicts={index.counts.get('CONFLICTS', 0)} "
        f"missing_refs={index.counts.get('MISSING_REFERENCES', 0)} "
        f"bound_hashes={index.counts.get('BOUND_HASHES', 0)} "
        f"unbound_hashes={index.counts.get('UNBOUND_HASHES', 0)} "
        f"hash_mismatches={index.counts.get('HASH_MISMATCHES', 0)} "
        f"device_access=0 modified_evidence=0"
    )
    print(f"markdown_out={markdown_path}")
    print(f"json_out={json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
