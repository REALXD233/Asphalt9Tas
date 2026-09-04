#!/usr/bin/env python3
"""Offline tests for check_a9_binary_format_registry_v1.py (P1).

Uses the real registry document plus temporary mutated Markdown copies.
Never touches a device and never writes into evidence/.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import check_a9_binary_format_registry_v1 as checker
from check_a9_binary_format_registry_v1 import check_registry, main

REAL_REGISTRY = checker.DEFAULT_REGISTRY


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def real_text() -> str:
    return REAL_REGISTRY.read_text(encoding="utf-8")


def _replace_row(text: str, name: str, transform) -> str:
    """Apply ``transform(row_text)`` to the table row whose first cell is name."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if cells and cells[0] == name:
            lines[index] = transform(line)
            return "\n".join(lines)
    raise AssertionError(f"row for {name} not found")


def _remove_row(text: str, name: str) -> str:
    lines = [line for line in text.splitlines()
             if not (line.startswith("|") and
                     line.strip().strip("|").split("|")[0].strip() == name)]
    return "\n".join(lines)


def _duplicate_row(text: str, name: str) -> str:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("|") and line.strip().strip("|").split("|")[0].strip() == name:
            lines.insert(index + 1, line)
            return "\n".join(lines)
    raise AssertionError(f"row for {name} not found")


class RegistryConsistencyTests(unittest.TestCase):
    def test_real_registry_is_consistent(self) -> None:
        result = check_registry(REAL_REGISTRY)
        self.assertTrue(result.consistent, result.errors)
        self.assertEqual(result.format_count, 28)

    def test_deleted_row_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_remove_row(real_text(), "A9UER1"), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("A9UER1" in error for error in result.errors))

    def test_duplicated_row_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_duplicate_row(real_text(), "A9NPS1"), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("duplicate" in error for error in result.errors))

    def test_wrong_version_fails(self) -> None:
        def bump(row: str) -> str:
            return row.replace("| 1 | 64 / 96 |", "| 2 | 64 / 96 |", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9NPS1", bump), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("version" in error for error in result.errors))

    def test_wrong_status_fails(self) -> None:
        def retag(row: str) -> str:
            return row.replace("`SUPPORTED`", "`KNOWN_UNSUPPORTED`", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9NPS1", retag), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("status" in error for error in result.errors))

    def test_companion_declared_on_non_usr_fails(self) -> None:
        def add_companion(row: str) -> str:
            return row.replace("| 无 | tick 连续", "| 可选显式 A9UTK1 | tick 连续", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9UER1", add_companion), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("A9UER1" in error and "companion" in error
                            for error in result.errors))

    def test_missing_usr_companion_declaration_fails(self) -> None:
        def remove_companion(row: str) -> str:
            return row.replace("可选显式 A9UTK1", "无", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9USR1", remove_companion), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("A9USR1" in error and "companion" in error
                            for error in result.errors))

    def test_fc1_auto_identification_claim_fails(self) -> None:
        def claim_auto(row: str) -> str:
            return row.replace("不用于自动识别", "可自动识别", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "FC1", claim_auto), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(
            any("automatic identification" in error for error in result.errors),
            result.errors,
        )

    def test_fc1_missing_no_auto_declaration_fails(self) -> None:
        def drop(row: str) -> str:
            return row.replace("不用于自动识别", "", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "FC1", drop), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(
            any("forbid automatic identification" in error for error in result.errors),
            result.errors,
        )

    def test_known_unsupported_version_mismatch_fails(self) -> None:
        def bump(row: str) -> str:
            return row.replace("`A9CDT1\\0\\0` | 1 |", "`A9CDT1\\0\\0` | 2 |", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9CDT1", bump), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("A9CDT1" in error and "version" in error
                            for error in result.errors), result.errors)

    def test_known_unsupported_na_version_mismatch_fails(self) -> None:
        def bump(row: str) -> str:
            return row.replace("`A9PST1\\0\\0` | na |", "`A9PST1\\0\\0` | 1 |", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9PST1", bump), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("A9PST1" in error for error in result.errors), result.errors)

    def test_removed_version_column_fails(self) -> None:
        def drop(row: str) -> str:
            return row.replace("| 1 | `tools/parse_conditional_audit_v1.py`",
                               "| `tools/parse_conditional_audit_v1.py`", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9CDT1", drop), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("A9CDT1" in error for error in result.errors), result.errors)

    def test_swapped_authority_paths_fail(self) -> None:
        def swap(row: str) -> str:
            return row.replace(
                "`tools/synchronized_tick_recording_v1.py`",
                "`tools/synchronized_brake_recording_v1.py`", 1,
            )

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "A9USR1", swap), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(
            any("A9USR1" in error and "authority" in error for error in result.errors),
            result.errors,
        )

    def test_fc1_missing_pid_base_declaration_fails(self) -> None:
        def drop_params(row: str) -> str:
            return row.replace("必须 `--format FC1 --pid <pid> --base <base>`", "必须 `--pid`", 1)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_replace_row(real_text(), "FC1", drop_params), encoding="utf-8")
            result = check_registry(path)
        self.assertFalse(result.consistent)
        self.assertTrue(any("FC1" in error for error in result.errors))

    def test_json_output_parseable(self) -> None:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(["--json"])
        self.assertEqual(code, 0)
        parsed = json.loads(buffer.getvalue())
        self.assertTrue(parsed["consistent"])
        self.assertEqual(parsed["format_count"], 28)
        self.assertTrue(parsed["read_only"])
        self.assertEqual(parsed["device_access"], 0)

    def test_real_registry_sha256_unchanged(self) -> None:
        before = sha256(REAL_REGISTRY.read_bytes())
        result = check_registry(REAL_REGISTRY)
        self.assertTrue(result.consistent)
        self.assertEqual(sha256(REAL_REGISTRY.read_bytes()), before)

    def test_unreadable_registry_exit_three(self) -> None:
        missing = Path(tempfile.gettempdir()) / "no_such_registry_9f31.md"
        code, out = self._run(["--registry", str(missing)])
        self.assertEqual(code, 3)
        self.assertIn("cannot read registry", out)

    def test_cli_consistent_exit_zero(self) -> None:
        code, out = self._run([])
        self.assertEqual(code, 0)
        self.assertIn("format_count=28", out)
        self.assertIn("read_only=1 device_access=0", out)

    def test_cli_drift_exit_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "registry.md"
            path.write_text(_remove_row(real_text(), "A9UTK1"), encoding="utf-8")
            code, out = self._run(["--registry", str(path)])
        self.assertEqual(code, 1)
        self.assertIn("REGISTRY_INCONSISTENT", out)

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


if __name__ == "__main__":
    unittest.main()
