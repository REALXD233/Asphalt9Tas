#!/usr/bin/env python3
"""Offline staleness audit for Android-port evidence documents.

The audit is intentionally conservative.  It does not rewrite any evidence
file.  Instead it groups evidence documents by a normalized topic key, compares
release dates and explicit status markers, and reports likely stale material
for human review.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import Iterable


WORKSPACE_ROOT = Path(__file__).resolve().parent.parent.parent
EVIDENCE_DIR = WORKSPACE_ROOT / "android-port" / "evidence"
DATE_RE = re.compile(r"\b(20\d{2})-(\d{2})-(\d{2})\b")
FILENAME_DATE_RE = re.compile(r"_(20\d{6})(?:_|\.)")
TITLE_RE = re.compile(r"^#\s+(.*)$", re.MULTILINE)

STATUS_MARKERS = {
    "LIVE_PASS": ("live pass", "live-pass", "live result", "live-result"),
    "OFFLINE_PASS": ("offline pass", "offline-pass"),
    "BUILD_ONLY": ("build-only", "build only", "build/design-only"),
    "FAIL_CLOSED": ("fail-closed", "fail closed", "rejected"),
    "RETIRED": ("permanently retired", "route permanently retired", "retired", "crash", "fatal"),
    "SUPERSEDED": ("superseded",),
}

SUFFIX_TOKENS = {
    "BUILD",
    "BUILD_ONLY",
    "OFFLINE",
    "OFFLINE_PASS",
    "LIVE",
    "LIVE_PASS",
    "LIVE_RESULT",
    "PASS",
    "RESULT",
    "PLAN",
    "DESIGN",
    "INCONCLUSIVE",
    "CRASH",
    "RETIRED",
    "FAILED",
    "FAIL",
    "TRY",
    "ATTEMPT",
    "PREPARE",
    "FRESH",
    "PROCESS",
    "GATE",
    "PASSIVE",
    "RUNNER",
    "RESULTS",
    "PASS_ONLY",
}

TOKEN_TRAIL_RE = re.compile(r"^(?:ATTEMPT\d+|RUN\d+|RESULT\d+|PASS\d+|BUILD_ONLY|BUILD|OFFLINE_PASS|OFFLINE|LIVE_PASS|LIVE_RESULT|LIVE|PASS|RESULT|PLAN|DESIGN|INCONCLUSIVE|CRASH|RETIRED|FAIL(?:ED)?|SUPERSEDED)$", re.I)


@dataclasses.dataclass(frozen=True)
class EvidenceDoc:
    file: str
    date: str | None
    topic_key: str
    status: str
    title: str | None
    declared_result: str | None
    superseded_marker: bool
    retired_marker: bool
    stale_reason: str | None = None


@dataclasses.dataclass(frozen=True)
class TopicGroup:
    topic_key: str
    latest_date: str | None
    docs: list[EvidenceDoc]


@dataclasses.dataclass(frozen=True)
class StalenessReport:
    generated_at: str
    workspace_root: str
    evidence_dir: str
    total_docs: int
    stale_docs: list[EvidenceDoc]
    topic_groups: list[TopicGroup]
    warnings: list[str]


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _extract_title(text: str) -> str | None:
    match = TITLE_RE.search(text)
    return match.group(1).strip() if match else None


def _extract_date(text: str, filename: str) -> str | None:
    match = DATE_RE.search(text)
    if match:
        return match.group(0)
    match = FILENAME_DATE_RE.search(filename)
    if match:
        raw = match.group(1)
        return f"{raw[:4]}-{raw[4:6]}-{raw[6:8]}"
    return None


def _extract_declared_result(title: str | None) -> str | None:
    if not title:
        return None
    parts = re.split(r"\s+[—-]\s+", title, maxsplit=1)
    return parts[1].strip() if len(parts) > 1 else None


def _classify_status(text: str, filename: str) -> str:
    combined = f"{filename.lower()}\n{text.lower()}"
    for status, markers in STATUS_MARKERS.items():
        if any(marker in combined for marker in markers):
            return status
    return "UNKNOWN"


def _normalize_topic_key(filename: str) -> str:
    stem = filename.rsplit(".", 1)[0]
    stem = re.sub(r"_(20\d{6})$", "", stem)
    parts = stem.split("_")
    while parts and TOKEN_TRAIL_RE.match(parts[-1]):
        parts.pop()
    while parts and parts[-1].isdigit():
        parts.pop()
    return "_".join(parts) if parts else stem


def _parse_date_sort_key(date: str | None) -> tuple[int, int, int]:
    if not date:
        return (0, 0, 0)
    return tuple(int(part) for part in date.split("-"))  # type: ignore[return-value]


def _has_explicit_staleness(text: str) -> bool:
    lower = text.lower()
    return any(token in lower for token in ("superseded", "retired", "current status", "supersedes", "supersede"))


def _iter_docs() -> Iterable[EvidenceDoc]:
    for path in sorted(EVIDENCE_DIR.glob("*.md")):
        text = _read_text(path)
        title = _extract_title(text)
        declared = _extract_declared_result(title)
        date = _extract_date(text, path.name)
        status = _classify_status(text, path.name)
        topic_key = _normalize_topic_key(path.stem)
        superseded_marker = "superseded" in text.lower() or "superseded" in path.name.lower()
        retired_marker = status == "RETIRED"
        yield EvidenceDoc(
            file=path.name,
            date=date,
            topic_key=topic_key,
            status=status,
            title=title,
            declared_result=declared,
            superseded_marker=superseded_marker,
            retired_marker=retired_marker,
        )


def build_report() -> StalenessReport:
    docs = list(_iter_docs())
    grouped: dict[str, list[EvidenceDoc]] = defaultdict(list)
    source_text_cache: dict[str, str] = {}
    for doc in docs:
        grouped[doc.topic_key].append(doc)

    stale: list[EvidenceDoc] = []
    topic_groups: list[TopicGroup] = []
    warnings: list[str] = []

    for topic_key, items in sorted(grouped.items()):
        latest = max(items, key=lambda item: _parse_date_sort_key(item.date))
        latest_date = latest.date
        topic_groups.append(TopicGroup(topic_key=topic_key, latest_date=latest_date, docs=sorted(items, key=lambda item: (_parse_date_sort_key(item.date), item.file))))

        if len(items) <= 1:
            continue

        for item in items:
            if item.file == latest.file:
                continue
            reason_parts: list[str] = []
            if item.date and latest_date and item.date < latest_date:
                reason_parts.append(f"older than latest {latest.file} ({latest_date})")
            if item.status == "UNKNOWN" and latest.status != "UNKNOWN":
                reason_parts.append(f"latest status is {latest.status}")
            if item.status in {"LIVE_PASS", "OFFLINE_PASS"} and latest.status in {"BUILD_ONLY", "FAIL_CLOSED", "RETIRED", "SUPERSEDED"}:
                reason_parts.append(f"later topic version is {latest.status}")
            if item.superseded_marker and latest.file != item.file:
                reason_parts.append("contains explicit superseded marker")
            if item.retired_marker and latest.status == "LIVE_PASS":
                reason_parts.append("retired artifact is older than a live-pass variant")
            if reason_parts:
                stale.append(dataclasses.replace(item, stale_reason="; ".join(reason_parts)))

    # Flag a few global stale patterns that are independent of topic grouping.
    for item in docs:
        if item.file.startswith("CURRENT_STATUS_"):
            continue
        text = _read_text(EVIDENCE_DIR / item.file)
        if _has_explicit_staleness(text) and item.status not in {"SUPERSEDED", "RETIRED"}:
            warnings.append(f"{item.file} contains staleness language but is classified as {item.status}")

    return StalenessReport(
        generated_at=os.environ.get("DOCUMENT_STALENESS_AUDIT_GENERATED_AT", "2026-08-18T00:00:00Z"),
        workspace_root=str(WORKSPACE_ROOT),
        evidence_dir=str(EVIDENCE_DIR),
        total_docs=len(docs),
        stale_docs=sorted(stale, key=lambda item: (_parse_date_sort_key(item.date), item.file)),
        topic_groups=topic_groups,
        warnings=warnings,
    )


def _render_markdown(report: StalenessReport) -> str:
    lines: list[str] = []
    lines.append("# Document staleness audit 2026-08-18")
    lines.append("")
    lines.append(f"Workspace root `{report.workspace_root}`")
    lines.append(f"Evidence dir `{report.evidence_dir}`")
    lines.append(f"Generated at {report.generated_at}")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(
        f"DOCUMENT_STALENESS_SUMMARY total_docs={report.total_docs} stale_docs={len(report.stale_docs)} warnings={len(report.warnings)} device_access=0"
    )
    lines.append("")
    lines.append("## Stale documents")
    lines.append("")
    if not report.stale_docs:
        lines.append("No stale documents detected by the conservative topic/date heuristic.")
    else:
        for item in report.stale_docs:
            lines.append(f"- `{item.file}` [{item.topic_key}] date={item.date or 'unknown'} status={item.status}")
            if item.stale_reason:
                lines.append(f"  - reason: {item.stale_reason}")
    lines.append("")
    lines.append("## Topic groups")
    lines.append("")
    lines.append("| Topic | Latest date | Documents |")
    lines.append("|---|---|---|")
    for group in report.topic_groups:
        docs = ", ".join(f"`{doc.file}`" for doc in group.docs)
        lines.append(f"| {group.topic_key} | {group.latest_date or ''} | {docs} |")
    lines.append("")
    if report.warnings:
        lines.append("## Warnings")
        lines.append("")
        for warning in report.warnings:
            lines.append(f"- {warning}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--markdown-out", type=Path, default=None)
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--stdout-only", action="store_true")
    args = parser.parse_args()

    report = build_report()
    md_path = args.markdown_out or (EVIDENCE_DIR / "DOCUMENT_STALENESS_AUDIT_20260818.md")
    json_path = args.json_out or (EVIDENCE_DIR / "DOCUMENT_STALENESS_AUDIT_20260818.json")

    markdown_text = _render_markdown(report)
    json_text = json.dumps(dataclasses.asdict(report), ensure_ascii=False, indent=2) + "\n"

    if not args.stdout_only:
        md_path.write_text(markdown_text, encoding="utf-8")
        json_path.write_text(json_text, encoding="utf-8")

    print(
        f"DOCUMENT_STALENESS_SUMMARY total_docs={report.total_docs} stale_docs={len(report.stale_docs)} "
        f"warnings={len(report.warnings)} device_access=0"
    )
    print(f"markdown_out={md_path}")
    print(f"json_out={json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
