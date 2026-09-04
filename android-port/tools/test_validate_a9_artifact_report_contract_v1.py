#!/usr/bin/env python3
"""Offline tests for validate_a9_artifact_report_contract_v1.py (P1).

Generates every inspector report status with the real inspector, mutates each
enum and cross-field invariant, and checks CLI JSON/text paths.  Never touches
a device; input JSON files stay byte-identical.
"""

from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import inspect_a9_artifact_v1 as inspector
import validate_a9_artifact_report_contract_v1 as contract
from validate_a9_artifact_report_contract_v1 import main, validate_report

from test_synchronized_tick_recording_v1 import make_capture
from test_inspect_a9_artifact_v1 import make_fc1, make_nps1, make_utk1


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_report(directory: Path, name: str, report: dict) -> Path:
    path = directory / name
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return path


def real_reports() -> dict[str, dict]:
    """Each inspector status generated with the real inspector."""
    usr1_report, usr1_recording = make_capture(3)
    reports = {
        "valid_nps1": inspector.inspect_bytes(make_nps1()),
        "structure_only_usr1": inspector.inspect_bytes(usr1_report),
        "cross_bound_usr1": inspector.inspect_bytes(
            usr1_report, companion_blob=usr1_recording
        ),
        "cross_bind_invalid": inspector.inspect_bytes(
            usr1_report, companion_blob=usr1_recording[:-3]
        ),
        "unsupported_unknown": inspector.inspect_bytes(b"????????"),
        "usage_error": inspector.inspect_bytes(make_nps1(), companion_blob=b"x" * 96),
    }
    return reports


class ValidContractTests(unittest.TestCase):
    def test_every_real_report_status_is_valid(self) -> None:
        reports = real_reports()
        for name, report in reports.items():
            with self.subTest(report=name):
                self.assertEqual(validate_report(report), [], report)
                self.assertEqual(report["read_only"], True)
                self.assertEqual(report["device_access"], 0)

    def test_cross_bound_invariants_hold(self) -> None:
        report = real_reports()["cross_bound_usr1"]
        self.assertTrue(report["capture_validated"])
        self.assertEqual(report["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertEqual(report["companion_status"], "VALID")
        self.assertEqual(report["companion_format"], "A9UTK1")
        self.assertIn("companion_cross_bind", report["checks"])

    def test_manifest_extended_fields_allowed(self) -> None:
        report = dict(real_reports()["structure_only_usr1"])
        report["id"] = "entry-1"
        report["path"] = "capture.a9usr1"
        report["main_sha256"] = sha256(b"main")
        report["companion_sha256"] = None
        self.assertEqual(validate_report(report), [])

    def test_fc1_report_contract_valid(self) -> None:
        report = inspector.inspect_bytes(make_fc1(), format_name="FC1",
                                         pid=1234, base=0x70000000)
        self.assertEqual(validate_report(report), [])


class InvalidContractTests(unittest.TestCase):
    def _mutate(self, report: dict, **changes) -> dict:
        changed = dict(report)
        changed.update(changes)
        return changed

    def test_each_enum_violation_fails(self) -> None:
        base = real_reports()["structure_only_usr1"]
        cases = {
            "status": self._mutate(base, status="MAYBE"),
            "scope": self._mutate(base, validation_scope="LIVE_PASS"),
            "companion_status": self._mutate(base, companion_status="PENDING"),
        }
        for name, report in cases.items():
            with self.subTest(case=name):
                self.assertTrue(validate_report(report), name)

    def test_cross_field_invariants_fail(self) -> None:
        base = real_reports()["structure_only_usr1"]
        cases = {
            "capture_true_on_structure": self._mutate(
                base, capture_validated=True),
            "valid_with_error": self._mutate(base, error="oops"),
            "invalid_without_error": self._mutate(
                base, status="INVALID", error=None),
            "invalid_with_checks": self._mutate(
                base, status="INVALID", error="bad", checks=["magic"]),
            "read_only_false": self._mutate(base, read_only=False),
            "device_access_1": self._mutate(base, device_access=1),
        }
        for name, report in cases.items():
            with self.subTest(case=name):
                self.assertTrue(validate_report(report), name)

    def test_cross_bound_downgrades_fail(self) -> None:
        report = real_reports()["cross_bound_usr1"]
        cases = {
            "wrong_format": self._mutate(report, format="A9UTK1"),
            "wrong_scope": self._mutate(report, validation_scope="STRUCTURE_ONLY"),
            "wrong_companion_status": self._mutate(report, companion_status="INVALID"),
            "wrong_companion_format": self._mutate(report, companion_format="A9NPS1"),
            "missing_check": self._mutate(
                report,
                checks=[c for c in report["checks"] if c != "companion_cross_bind"]),
        }
        for name, changed in cases.items():
            with self.subTest(case=name):
                self.assertTrue(validate_report(changed), name)

    def test_bool_as_int_fails(self) -> None:
        base = real_reports()["structure_only_usr1"]
        for field in ("version", "size", "frames", "first_tick", "last_tick"):
            with self.subTest(field=field):
                changed = self._mutate(base, **{field: True})
                self.assertTrue(validate_report(changed), field)

    def test_bad_hashes_fail(self) -> None:
        base = dict(real_reports()["structure_only_usr1"])
        for bad in ("short", "Z" * 64, "123", 42):
            with self.subTest(bad=bad):
                changed = dict(base, main_sha256=bad)
                self.assertTrue(validate_report(changed), bad)

    def test_missing_required_field_fails(self) -> None:
        base = real_reports()["structure_only_usr1"]
        del base["checks"]
        self.assertTrue(validate_report(base))


class ContractCliTests(unittest.TestCase):
    def _run(self, argv: list[str]) -> tuple[int, str, str]:
        buffer = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(buffer), redirect_stderr(err):
            code = main(argv)
        return code, buffer.getvalue(), err.getvalue()

    def test_cli_valid_text_and_json(self) -> None:
        report = real_reports()["cross_bound_usr1"]
        with tempfile.TemporaryDirectory() as tmp:
            path = write_report(Path(tmp), "ok.json", report)
            code, out, _ = self._run([str(path)])
            code_json, out_json, _ = self._run([str(path), "--json"])
        self.assertEqual(code, 0)
        self.assertIn("A9_REPORT_CONTRACT_VALID", out)
        self.assertIn("read_only=1 device_access=0", out)
        self.assertEqual(code_json, 0)
        parsed = json.loads(out_json)
        self.assertTrue(parsed["valid"])
        self.assertTrue(parsed["read_only"])
        self.assertEqual(parsed["device_access"], 0)

    def test_cli_invalid_exit_one(self) -> None:
        report = dict(real_reports()["structure_only_usr1"], status="NOPE", error="x")
        with tempfile.TemporaryDirectory() as tmp:
            path = write_report(Path(tmp), "bad.json", report)
            code, out, _ = self._run([str(path)])
            code_json, out_json, _ = self._run([str(path), "--json"])
        self.assertEqual(code, 1)
        self.assertIn("A9_REPORT_CONTRACT_INVALID", out)
        self.assertEqual(code_json, 1)
        parsed = json.loads(out_json)
        self.assertFalse(parsed["valid"])
        self.assertTrue(parsed["errors"])

    def test_cli_unreadable_exit_three(self) -> None:
        code, out, _ = self._run([str(Path(tempfile.gettempdir()) / "nope_7f21.json")])
        self.assertEqual(code, 3)
        self.assertIn("cannot read report", out)

    def test_cli_invalid_json_exit_three(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.json"
            path.write_text("{nope", encoding="utf-8")
            code, out, _ = self._run([str(path)])
            code_json, out_json, _ = self._run([str(path), "--json"])
        self.assertEqual(code, 3)
        self.assertIn("not valid JSON", out)
        self.assertEqual(code_json, 3)
        parsed = json.loads(out_json)
        self.assertFalse(parsed["valid"])

    def test_input_json_byte_identical(self) -> None:
        report = real_reports()["structure_only_usr1"]
        with tempfile.TemporaryDirectory() as tmp:
            path = write_report(Path(tmp), "r.json", report)
            before = sha256(path.read_bytes())
            self._run([str(path)])
            self._run([str(path), "--json"])
            self.assertEqual(sha256(path.read_bytes()), before)


if __name__ == "__main__":
    unittest.main()
