#!/usr/bin/env python3
"""Unified artifact report contract validator (P1/N4-R1).

This tool validates the FIELD TYPES and CROSS-FIELD INVARIANTS of an inspector
JSON report (``inspect_a9_artifact_v1.py`` output, including entries produced
by the manifest inspector).  It never re-parses binary artifacts and never
re-validates the underlying files.

Exit codes:
  0  contract valid
  1  JSON readable but the contract is violated
  3  file unreadable / JSON not parseable / CLI usage error
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

EXIT_OK = 0
EXIT_INVALID = 1
EXIT_IO_OR_USAGE = 3

VALID_STATUSES = ("VALID", "INVALID", "UNSUPPORTED", "IO_ERROR", "USAGE_ERROR")
VALID_SCOPES = ("STRUCTURE_ONLY", "CAPTURE_CROSS_BOUND")
VALID_COMPANION_STATUSES = (
    "NOT_APPLICABLE", "NOT_PROVIDED", "NOT_EVALUATED", "IO_ERROR", "INVALID", "VALID",
)
COMPANION_FORMATS = frozenset({"A9USR1", "A9USR2", "A9USR3", "A9USR4"})

REQUIRED_FIELDS = (
    "status", "format", "version", "size", "frames", "first_tick", "last_tick",
    "checks", "validation_scope", "companion_status", "companion_format",
    "capture_validated", "read_only", "device_access", "error",
)

HEX64_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def _strict_int_or_none(value: Any, name: str, errors: list[str]) -> None:
    if value is None:
        return
    if isinstance(value, bool) or not isinstance(value, int):
        errors.append(f"{name} must be an int or null, got {type(value).__name__}")


def validate_report(report: Any) -> list[str]:
    """Return a list of contract violations (empty means valid)."""
    errors: list[str] = []
    if not isinstance(report, dict):
        return ["report must be a JSON object"]

    for field in REQUIRED_FIELDS:
        if field not in report:
            errors.append(f"missing required field: {field}")
    if errors:
        return errors

    status = report["status"]
    if status not in VALID_STATUSES:
        errors.append(f"invalid status {status!r}")

    scope = report["validation_scope"]
    if scope not in VALID_SCOPES:
        errors.append(f"invalid validation_scope {scope!r}")

    companion_status = report["companion_status"]
    if companion_status not in VALID_COMPANION_STATUSES:
        errors.append(f"invalid companion_status {companion_status!r}")

    if report["read_only"] is not True:
        errors.append("read_only must be true")
    if report["device_access"] != 0 or isinstance(report["device_access"], bool):
        errors.append("device_access must be 0")

    if not isinstance(report["capture_validated"], bool):
        errors.append("capture_validated must be a boolean")

    for field in ("format", "companion_format"):
        value = report[field]
        if value is not None and not isinstance(value, str):
            errors.append(f"{field} must be a string or null")

    for field in ("version", "size", "frames", "first_tick", "last_tick"):
        _strict_int_or_none(report[field], field, errors)

    if not isinstance(report["checks"], list):
        errors.append("checks must be a list")
    elif not all(isinstance(item, str) for item in report["checks"]):
        errors.append("checks must be a list of strings")

    error_value = report["error"]
    if status == "VALID":
        if error_value is not None:
            errors.append("status=VALID requires error=null")
    elif not isinstance(error_value, str) or not error_value:
        errors.append(f"status={status} requires a non-empty error string")

    if status != "VALID" and report["checks"]:
        errors.append("non-VALID reports must have empty checks")

    # capture_validated=true cross-field invariants.
    if report["capture_validated"]:
        if status != "VALID":
            errors.append("capture_validated=true requires status=VALID")
        if report["format"] not in COMPANION_FORMATS:
            errors.append("capture_validated=true requires an A9USR1-A9USR4 format")
        if scope != "CAPTURE_CROSS_BOUND":
            errors.append("capture_validated=true requires validation_scope=CAPTURE_CROSS_BOUND")
        if companion_status != "VALID":
            errors.append("capture_validated=true requires companion_status=VALID")
        if report["companion_format"] != "A9UTK1":
            errors.append("capture_validated=true requires companion_format=A9UTK1")
        if "companion_cross_bind" not in report["checks"]:
            errors.append("capture_validated=true requires checks to contain companion_cross_bind")

    # STRUCTURE_ONLY invariants.
    if scope == "STRUCTURE_ONLY":
        if report["capture_validated"]:
            errors.append("STRUCTURE_ONLY reports must have capture_validated=false")
        if "companion_cross_bind" in report["checks"]:
            errors.append("STRUCTURE_ONLY reports must not claim companion_cross_bind")

    # Optional identity hashes: null or 64-hex only.
    for field in ("main_sha256", "companion_sha256"):
        value = report.get(field)
        if value is None:
            continue
        if not isinstance(value, str) or not HEX64_RE.fullmatch(value):
            errors.append(f"{field} must be null or a 64-char hex string")

    return errors


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:  # pragma: no cover - argparse path
        self.print_usage(sys.stderr)
        print(f"contract_error: {message}", file=sys.stderr)
        raise SystemExit(EXIT_IO_OR_USAGE)


def _render_text(errors: list[str]) -> str:
    if not errors:
        return "A9_REPORT_CONTRACT_VALID read_only=1 device_access=0"
    lines = [f"A9_REPORT_CONTRACT_INVALID error={errors[0]} read_only=1 device_access=0"]
    lines.extend(f"contract_error: {error}" for error in errors[1:])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="inspector JSON report file")
    parser.add_argument("--json", action="store_true", help="emit JSON on stdout")
    args = parser.parse_args(argv)

    try:
        text = args.report.read_text(encoding="utf-8")
    except OSError as error:
        if args.json:
            print(json.dumps({
                "valid": False,
                "error": f"cannot read report: {error}",
                "errors": [f"cannot read report: {error}"],
                "read_only": True,
                "device_access": 0,
            }, ensure_ascii=False, indent=2))
        else:
            print(f"A9_REPORT_CONTRACT_INVALID error=cannot read report: {error} "
                  "read_only=1 device_access=0")
        return EXIT_IO_OR_USAGE

    try:
        report = json.loads(text)
    except json.JSONDecodeError as error:
        if args.json:
            print(json.dumps({
                "valid": False,
                "error": f"report is not valid JSON: {error}",
                "errors": [f"report is not valid JSON: {error}"],
                "read_only": True,
                "device_access": 0,
            }, ensure_ascii=False, indent=2))
        else:
            print(f"A9_REPORT_CONTRACT_INVALID error=report is not valid JSON "
                  "read_only=1 device_access=0")
        return EXIT_IO_OR_USAGE

    errors = validate_report(report)
    if args.json:
        print(json.dumps({
            "valid": not errors,
            "errors": errors,
            "read_only": True,
            "device_access": 0,
        }, ensure_ascii=False, indent=2))
    else:
        print(_render_text(errors))
    return EXIT_OK if not errors else EXIT_INVALID


if __name__ == "__main__":
    raise SystemExit(main())
