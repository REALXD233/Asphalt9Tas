#!/usr/bin/env python3
"""Offline tests for inspect_a9_artifact_manifest_v1.py (P2).

All fixtures are synthesized in temporary directories; the manifest inspector
is exercised in-process.  No device access, no evidence writes.
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

import inspect_a9_artifact_manifest_v1 as manifest_tool
from inspect_a9_artifact_manifest_v1 import run_manifest, main

from test_synchronized_tick_recording_v1 import make_capture
from test_synchronized_action_until_release_recording_v1 import make_action_until_release
from test_inspect_a9_artifact_v1 import make_fc1, make_nps1


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def write_manifest(directory: Path, entries: list[dict]) -> Path:
    payload = {"schema": "A9_ARTIFACT_MANIFEST_V1", "entries": entries}
    return write(directory / "manifest.json",
                 json.dumps(payload, ensure_ascii=False).encode("utf-8"))


def make_workspace(tmp: str) -> dict[str, Path]:
    directory = Path(tmp)
    report, recording = make_capture(3)
    r4, c4 = make_action_until_release()
    files = {
        "cap1.a9usr1": write(directory / "cap1.a9usr1", report),
        "cap1.a9utk1": write(directory / "cap1.a9utk1", recording),
        "cap4.a9usr4": write(directory / "cap4.a9usr4", r4),
        "cap4.a9utk1": write(directory / "cap4.a9utk1", c4),
        "fc1.bin": write(directory / "fc1.bin", make_fc1(1234, 0x70000000)),
        "nps.bin": write(directory / "nps.bin", make_nps1()),
    }
    return {"dir": directory, **files}


class ManifestValidTests(unittest.TestCase):
    def test_valid_batch_with_cross_bound_and_fc1(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "source-1", "path": "cap1.a9usr1", "companion": "cap1.a9utk1"},
                {"id": "source-4", "path": "cap4.a9usr4", "companion": "cap4.a9utk1"},
                {"id": "fc1", "path": "fc1.bin", "format": "FC1",
                 "pid": 1234, "base": "0x70000000"},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertIsNone(fatal)
        self.assertEqual(summary["exit_code"], 0)
        self.assertEqual(summary["total"], 3)
        self.assertEqual(summary["valid"], 3)
        by_id = {r["id"]: r for r in reports}
        self.assertEqual(by_id["source-1"]["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertTrue(by_id["source-1"]["capture_validated"])
        self.assertEqual(by_id["source-4"]["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertEqual(by_id["fc1"]["status"], "VALID")
        self.assertEqual(by_id["fc1"]["validation_scope"], "STRUCTURE_ONLY")

    def test_structure_only_entry_without_companion(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "solo", "path": "cap1.a9usr1"},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertEqual(summary["exit_code"], 0)
        self.assertEqual(reports[0]["validation_scope"], "STRUCTURE_ONLY")
        self.assertEqual(reports[0]["companion_status"], "NOT_PROVIDED")
        self.assertFalse(reports[0]["capture_validated"])

    def test_absolute_paths_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "abs", "path": str(ws["cap1.a9usr1"]),
                 "companion": str(ws["cap1.a9utk1"])},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertEqual(summary["exit_code"], 0)
        self.assertEqual(reports[0]["validation_scope"], "CAPTURE_CROSS_BOUND")


class ManifestFailureTests(unittest.TestCase):
    def test_invalid_entry_does_not_stop_later_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            bad = write(ws["dir"] / "bad.a9usr1", ws["cap1.a9usr1"].read_bytes()[:-3])
            manifest = write_manifest(ws["dir"], [
                {"id": "broken", "path": "bad.a9usr1"},
                {"id": "good", "path": "cap1.a9usr1", "companion": "cap1.a9utk1"},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertIsNone(fatal)
        self.assertEqual(summary["exit_code"], 1)
        self.assertEqual(summary["invalid"], 1)
        self.assertEqual(summary["valid"], 1)
        self.assertEqual(reports[1]["status"], "VALID")

    def test_exit_code_priority_io_over_unsupported_over_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "missing", "path": "no_such_file.bin"},
                {"id": "unknown", "path": str(write(ws["dir"] / "u.bin", b"????????"))},
                {"id": "broken", "path": str(write(ws["dir"] / "b.bin",
                                                  ws["cap1.a9usr1"].read_bytes()[:-3]))},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertEqual(summary["exit_code"], 3)
        self.assertEqual(summary["io_error"], 1)
        self.assertEqual(summary["unsupported"], 1)
        self.assertEqual(summary["invalid"], 1)

    def test_non_fc1_with_format_pid_base_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "bad", "path": "cap1.a9usr1", "format": "A9NPS1",
                 "pid": 1234, "base": 0x70000000},
            ])
            _, _, fatal = run_manifest(manifest)
        self.assertIsNotNone(fatal)
        self.assertIn("format/pid/base are only allowed for FC1", fatal)

    def test_schema_mismatch_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            path = ws["dir"] / "manifest.json"
            path.write_text(json.dumps({"schema": "WRONG", "entries": []}), encoding="utf-8")
            _, _, fatal = run_manifest(path)
        self.assertIsNotNone(fatal)
        self.assertIn("schema", fatal)

    def test_unknown_keys_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "x", "path": "cap1.a9usr1", "surprise": 1},
            ])
            _, _, fatal = run_manifest(manifest)
        self.assertIsNotNone(fatal)
        self.assertIn("unknown keys", fatal)

    def test_empty_entries_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps({"schema": "A9_ARTIFACT_MANIFEST_V1",
                                        "entries": []}), encoding="utf-8")
            _, _, fatal = run_manifest(path)
        self.assertIsNotNone(fatal)
        self.assertIn("entries count", fatal)

    def test_too_many_entries_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            entries = [{"id": f"e{i}", "path": "x.bin"} for i in range(257)]
            path.write_text(json.dumps({"schema": "A9_ARTIFACT_MANIFEST_V1",
                                        "entries": entries}), encoding="utf-8")
            _, _, fatal = run_manifest(path)
        self.assertIsNotNone(fatal)
        self.assertIn("entries count", fatal)

    def test_duplicate_and_invalid_ids_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            dup = write_manifest(ws["dir"], [
                {"id": "same", "path": "cap1.a9usr1"},
                {"id": "same", "path": "cap4.a9usr4"},
            ])
            _, _, fatal_dup = run_manifest(dup)
            bad_id = write_manifest(ws["dir"], [
                {"id": "bad id!", "path": "cap1.a9usr1"},
            ])
            _, _, fatal_id = run_manifest(bad_id)
        self.assertIn("duplicate entry id", fatal_dup)
        self.assertIn("id must be", fatal_id)

    def test_malformed_json_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text("{not json", encoding="utf-8")
            _, _, fatal = run_manifest(path)
        self.assertIsNotNone(fatal)
        self.assertIn("not valid JSON", fatal)

    def test_missing_manifest_file_fails(self) -> None:
        _, _, fatal = run_manifest(Path(tempfile.gettempdir()) / "nope_9f12.json")
        self.assertIsNotNone(fatal)
        self.assertIn("cannot read manifest", fatal)


class ManifestStrictSchemaTests(unittest.TestCase):
    """Batch-4 P0 F4/F5: empty companion, bool-as-int, NUL and glob rejection."""

    def _fatal(self, entry: dict) -> str | None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            payload = {"schema": "A9_ARTIFACT_MANIFEST_V1", "entries": [entry]}
            path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
            _, _, fatal = run_manifest(path)
        return fatal

    def test_empty_companion_rejected(self) -> None:
        fatal = self._fatal({"id": "e", "path": "x.bin", "companion": ""})
        self.assertIsNotNone(fatal)
        self.assertIn("companion", fatal)

    def test_whitespace_companion_rejected(self) -> None:
        fatal = self._fatal({"id": "e", "path": "x.bin", "companion": "   "})
        self.assertIsNotNone(fatal)
        self.assertIn("companion", fatal)

    def test_nul_in_path_rejected(self) -> None:
        fatal = self._fatal({"id": "e", "path": "x\x00.bin"})
        self.assertIsNotNone(fatal)
        self.assertIn("NUL", fatal)

    def test_nul_in_companion_rejected(self) -> None:
        fatal = self._fatal({"id": "e", "path": "x.bin", "companion": "c\x00.bin"})
        self.assertIsNotNone(fatal)
        self.assertIn("NUL", fatal)

    def test_glob_shape_rejected(self) -> None:
        for bad in ("*.bin", "a?.bin", "[ab].bin", "d[0-9].bin"):
            with self.subTest(path=bad):
                fatal = self._fatal({"id": "e", "path": bad})
                self.assertIsNotNone(fatal)
                self.assertIn("glob", fatal)

    def test_boolean_pid_and_base_rejected(self) -> None:
        fatal_pid = self._fatal({"id": "e", "path": "x.bin", "format": "FC1",
                                 "pid": True, "base": 0x70000000})
        self.assertIsNotNone(fatal_pid)
        self.assertIn("pid", fatal_pid)
        fatal_base = self._fatal({"id": "e", "path": "x.bin", "format": "FC1",
                                  "pid": 1234, "base": True})
        self.assertIsNotNone(fatal_base)
        self.assertIn("base", fatal_base)

    def test_pid_and_base_bounds_enforced(self) -> None:
        fatal_pid_zero = self._fatal({"id": "e", "path": "x.bin", "format": "FC1",
                                      "pid": 0, "base": 1})
        self.assertIsNotNone(fatal_pid_zero)
        self.assertIn("pid", fatal_pid_zero)
        fatal_pid_high = self._fatal({"id": "e", "path": "x.bin", "format": "FC1",
                                      "pid": 0x100000000, "base": 1})
        self.assertIsNotNone(fatal_pid_high)
        self.assertIn("pid", fatal_pid_high)
        fatal_base_zero = self._fatal({"id": "e", "path": "x.bin", "format": "FC1",
                                       "pid": 1, "base": 0})
        self.assertIsNotNone(fatal_base_zero)
        self.assertIn("base", fatal_base_zero)

    def test_string_base_parsed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "fc1", "path": "fc1.bin", "format": "FC1",
                 "pid": 1234, "base": "0x70000000"},
            ])
            _, summary, fatal = run_manifest(manifest)
        self.assertIsNone(fatal)
        self.assertEqual(summary["exit_code"], 0)


class ManifestJsonFailureTests(unittest.TestCase):
    """Batch-4 P0 F7: --json must emit parseable JSON on every fatal path."""

    def test_json_fatal_paths_emit_parseable_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            missing = directory / "missing.json"
            not_json = directory / "not.json"
            not_json.write_text("{oops", encoding="utf-8")
            bad_schema = directory / "bad.json"
            bad_schema.write_text(json.dumps({"schema": "WRONG", "entries": []}),
                                  encoding="utf-8")
            bad_utf8 = directory / "bad_utf8.json"
            bad_utf8.write_bytes(b"\xff\xfe\x00bad")
            for name, path in (("not json", not_json), ("bad schema", bad_schema),
                               ("missing file", missing), ("bad utf8", bad_utf8)):
                buffer = io.StringIO()
                with redirect_stdout(buffer):
                    code = main([str(path), "--json"])
                self.assertEqual(code, 3, name)
                parsed = json.loads(buffer.getvalue())
                self.assertEqual(parsed["schema"], "A9_ARTIFACT_MANIFEST_V1")
                self.assertEqual(parsed["status"], "ERROR")
                self.assertTrue(parsed["error"])
                self.assertEqual(parsed["summary"]["exit_code"], 3)
                self.assertTrue(parsed["read_only"])
                self.assertEqual(parsed["device_access"], 0)
                self.assertFalse(parsed["auto_discovery"])


class ManifestSourceTests(unittest.TestCase):
    """Batch-4 P0 F6: exactly one entry block and full-length text hashes."""

    def test_single_main_entry_block(self) -> None:
        source = (TOOLS_DIR / "inspect_a9_artifact_manifest_v1.py").read_text(
            encoding="utf-8"
        )
        self.assertEqual(source.count('if __name__ == "__main__":'), 1)

    def test_text_output_uses_full_sha256(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            before_main = sha256(ws["cap1.a9usr1"].read_bytes())
            before_comp = sha256(ws["cap1.a9utk1"].read_bytes())
            manifest = write_manifest(ws["dir"], [
                {"id": "source-1", "path": "cap1.a9usr1", "companion": "cap1.a9utk1"},
            ])
            code, text = self._run([str(manifest)])
        self.assertEqual(code, 0)
        self.assertIn(f"main_sha256={before_main}", text)
        self.assertIn(f"companion_sha256={before_comp}", text)
        self.assertNotIn("...", text.splitlines()[1])

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


class ManifestSafetyTests(unittest.TestCase):
    def test_non_usr_entry_does_not_read_companion(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "nps", "path": "nps.bin", "companion": "no_such_companion.a9utk1"},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertIsNone(fatal)
        self.assertEqual(summary["exit_code"], 3)
        self.assertEqual(reports[0]["status"], "USAGE_ERROR")
        self.assertNotIn("cannot read companion", reports[0]["error"])

    def test_no_auto_discovery_of_adjacent_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            # Same directory holds a matching-looking recording; manifest omits it.
            manifest = write_manifest(ws["dir"], [
                {"id": "solo", "path": "cap1.a9usr1"},
            ])
            reports, summary, fatal = run_manifest(manifest)
        self.assertEqual(summary["exit_code"], 0)
        self.assertEqual(reports[0]["companion_status"], "NOT_PROVIDED")
        self.assertIsNone(reports[0]["companion_sha256"])

    def test_inputs_byte_identical_and_hashes_recorded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            before_main = sha256(ws["cap1.a9usr1"].read_bytes())
            before_comp = sha256(ws["cap1.a9utk1"].read_bytes())
            manifest = write_manifest(ws["dir"], [
                {"id": "source-1", "path": "cap1.a9usr1", "companion": "cap1.a9utk1"},
            ])
            before_manifest = sha256(manifest.read_bytes())
            reports, summary, fatal = run_manifest(manifest)
            self.assertIsNone(fatal)
            self.assertEqual(summary["exit_code"], 0)
            self.assertEqual(reports[0]["main_sha256"], before_main)
            self.assertEqual(reports[0]["companion_sha256"], before_comp)
            self.assertEqual(before_manifest, sha256(manifest.read_bytes()))
            self.assertEqual(sha256(ws["cap1.a9usr1"].read_bytes()), before_main)
            self.assertEqual(sha256(ws["cap1.a9utk1"].read_bytes()), before_comp)

    def test_cli_text_and_json_outputs_stable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            manifest = write_manifest(ws["dir"], [
                {"id": "source-1", "path": "cap1.a9usr1", "companion": "cap1.a9utk1"},
                {"id": "fc1", "path": "fc1.bin", "format": "FC1",
                 "pid": 1234, "base": "0x70000000"},
            ])
            code, text = self._run([str(manifest)])
            code_json, out_json = self._run([str(manifest), "--json"])
        self.assertEqual(code, 0)
        self.assertIn("A9_MANIFEST_SUMMARY", text)
        self.assertIn("read_only=1 device_access=0 auto_discovery=0", text)
        self.assertIn("capture_validated=true", text)
        self.assertEqual(code_json, 0)
        parsed = json.loads(out_json)
        self.assertEqual(parsed["schema"], "A9_ARTIFACT_MANIFEST_V1")
        self.assertEqual(parsed["summary"]["valid"], 2)
        self.assertTrue(parsed["read_only"])
        self.assertEqual(parsed["device_access"], 0)
        self.assertFalse(parsed["auto_discovery"])
        self.assertEqual(len(parsed["entries"]), 2)

    def test_cli_schema_error_exit_three(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps({"schema": "BAD", "entries": []}), encoding="utf-8")
            buffer = io.StringIO()
            err = io.StringIO()
            with redirect_stdout(buffer), redirect_stderr(err):
                code = main([str(path)])
        self.assertEqual(code, 3)
        self.assertIn("manifest schema error", err.getvalue())

    def test_no_uncaught_tracebacks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = make_workspace(tmp)
            bad = write(ws["dir"] / "bad.a9usr1", ws["cap1.a9usr1"].read_bytes()[:-3])
            manifest = write_manifest(ws["dir"], [
                {"id": "broken", "path": "bad.a9usr1"},
                {"id": "missing", "path": "no_such.bin"},
                {"id": "good", "path": "cap1.a9usr1", "companion": "cap1.a9utk1"},
            ])
            buffer = io.StringIO()
            err = io.StringIO()
            try:
                with redirect_stdout(buffer), redirect_stderr(err):
                    main([str(manifest)])
            except SystemExit:
                pass
            buffer2 = io.StringIO()
            err2 = io.StringIO()
            try:
                with redirect_stdout(buffer2), redirect_stderr(err2):
                    main([str(manifest), "--json"])
            except SystemExit:
                pass

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


if __name__ == "__main__":
    unittest.main()
