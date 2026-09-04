#!/usr/bin/env python3
"""Offline unit tests for run_offline_regression_suite_v1.py.

Covers the fail-closed behaviors required by the B2 work package:
1. a nonexistent group returns non-zero;
2. a missing allowlist file is recorded as FAIL;
3. a unittest module with 0 test cases is recorded as FAIL;
4. script entries are invoked with their full required arguments;
5. a failing entry does not stop the remaining entries;
6. the summary and the final exit code agree.

All tests use mocks and temporary directories; no device or live runner is
ever touched.
"""

from __future__ import annotations

import io
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import run_offline_regression_suite_v1 as runner


class FakeCompletedProcess:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def make_fake_run(returncodes):
    """Return (fake_run, calls) where fake_run records every cmd list."""
    calls: list[list[str]] = []
    codes = list(returncodes) if isinstance(returncodes, (list, tuple)) else None

    def fake_run(cmd, **kwargs):
        calls.append(list(cmd))
        code = codes.pop(0) if codes else 0
        return FakeCompletedProcess(code, stdout="fake out", stderr="")

    return fake_run, calls


class EmptyGroupTest(unittest.TestCase):
    def test_missing_group_returns_nonzero(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err):
            code = runner.main(["--stdout-only", "--group", "__definitely_missing_group__"])
        self.assertNotEqual(code, 0)
        self.assertIn("matched 0 allowlisted tests", err.getvalue())


class MissingAllowlistTest(unittest.TestCase):
    def test_missing_allowlist_file_is_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp) / "tools"
            tools.mkdir()
            with mock.patch.object(runner, "TOOLS_DIR", tools):
                results = runner.run_suite(
                    [("no_such_module_v1", "label", "group", runner.SC)]
                )
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "FAIL")
        self.assertEqual(results[0].exit_code, 2)
        self.assertIn("missing test file", results[0].output_tail)


class ZeroUnitTestsTest(unittest.TestCase):
    def test_zero_unittest_cases_is_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp) / "tools"
            tools.mkdir()
            (tools / "empty_tests_v1.py").write_text(
                "# module with no unittest.TestCase\n", encoding="utf-8"
            )
            sys.path.insert(0, str(tools))
            try:
                with mock.patch.object(runner, "TOOLS_DIR", tools):
                    results = runner.run_suite(
                        [("empty_tests_v1", "label", "group", runner.UT)]
                    )
            finally:
                sys.path.remove(str(tools))
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "FAIL")
        self.assertEqual(results[0].exit_code, 1)
        self.assertIn("no tests found", results[0].output_tail)


class ScriptArgumentsTest(unittest.TestCase):
    def _workspace(self, tmp: str) -> tuple[Path, Path]:
        ws = Path(tmp)
        tools = ws / "tools"
        tools.mkdir()
        # Required artifact files, resolved relative to the workspace root.
        for rel in (
            "android-port/build/frame-callback-bootstrap-v1/liba9tas_frame_callback_bootstrap_v1_build_only.so",
            "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
            "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
        ):
            path = ws / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"artifact")
        return ws, tools

    def test_script_entry_uses_full_artifact_args(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws, tools = self._workspace(tmp)
            (tools / "test_frame_callback_bootstrap_policy_v1.py").write_text(
                "import sys\n", encoding="utf-8"
            )
            fake_run, calls = make_fake_run([0])
            with mock.patch.object(runner, "TOOLS_DIR", tools), \
                 mock.patch.object(runner, "WORKSPACE_ROOT", ws), \
                 mock.patch.object(subprocess, "run", fake_run):
                results = runner.run_suite(
                    [("test_frame_callback_bootstrap_policy_v1", "FC-0", "g", runner.SC)]
                )
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "PASS")
        self.assertEqual(len(calls), 1)
        cmd = calls[0]
        self.assertEqual(cmd[0], runner.PY)
        self.assertTrue(cmd[1].endswith("test_frame_callback_bootstrap_policy_v1.py"))
        self.assertEqual(
            cmd[2:],
            [
                str(ws / "android-port/build/frame-callback-bootstrap-v1/liba9tas_frame_callback_bootstrap_v1_build_only.so"),
                str(ws / "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"),
                str(ws / "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe"),
            ],
        )
        # The displayed command keeps the workspace-relative artifact paths.
        self.assertIn(
            "android-port/build/frame-callback-bootstrap-v1/"
            "liba9tas_frame_callback_bootstrap_v1_build_only.so",
            results[0].command,
        )

    def test_validate_fc1_report_keeps_selftest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws = Path(tmp)
            tools = ws / "tools"
            tools.mkdir()
            (tools / "validate_fc1_report_v1.py").write_text("import sys\n", encoding="utf-8")
            fake_run, calls = make_fake_run([0])
            with mock.patch.object(runner, "TOOLS_DIR", tools), \
                 mock.patch.object(runner, "WORKSPACE_ROOT", ws), \
                 mock.patch.object(subprocess, "run", fake_run):
                results = runner.run_suite(
                    [("validate_fc1_report_v1", "report", "g", runner.SC)]
                )
        self.assertEqual(results[0].status, "PASS")
        self.assertIn("--selftest", calls[0])
        self.assertIn("--selftest", results[0].command)

    def test_missing_required_artifact_is_fail_not_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ws, tools = self._workspace(tmp)
            (tools / "test_frame_callback_bootstrap_policy_v1.py").write_text(
                "import sys\n", encoding="utf-8"
            )
            # Remove one required artifact so the entry must FAIL.
            (ws / "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe").unlink()
            fake_run, calls = make_fake_run([0])
            with mock.patch.object(runner, "TOOLS_DIR", tools), \
                 mock.patch.object(runner, "WORKSPACE_ROOT", ws), \
                 mock.patch.object(subprocess, "run", fake_run):
                results = runner.run_suite(
                    [("test_frame_callback_bootstrap_policy_v1", "FC-0", "g", runner.SC)]
                )
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "FAIL")
        self.assertEqual(results[0].exit_code, 2)
        self.assertIn("required artifact missing", results[0].output_tail)
        self.assertNotEqual(results[0].status, "SKIPPED")
        self.assertEqual(calls, [], "script must not run without its artifacts")


class ContinueAfterFailureTest(unittest.TestCase):
    def test_failure_does_not_stop_remaining_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp) / "tools"
            tools.mkdir()
            (tools / "failing_v1.py").write_text("import sys\n", encoding="utf-8")
            (tools / "passing_v1.py").write_text("import sys\n", encoding="utf-8")
            fake_run, calls = make_fake_run([1, 0])
            with mock.patch.object(runner, "TOOLS_DIR", tools), \
                 mock.patch.object(subprocess, "run", fake_run):
                results = runner.run_suite(
                    [
                        ("failing_v1", "failing", "g", runner.SC),
                        ("passing_v1", "passing", "g", runner.SC),
                    ]
                )
        self.assertEqual(len(results), 2)
        self.assertEqual(results[0].status, "FAIL")
        self.assertEqual(results[1].status, "PASS")


class SummaryExitCodeTest(unittest.TestCase):
    def _result(self, module: str, status: str) -> runner.TestResult:
        return runner.TestResult(
            module=module,
            label=module,
            group="g",
            kind=runner.SC,
            command=f"python {module}.py",
            exit_code=0 if status == "PASS" else 1,
            duration_ms=1,
            status=status,
            output_tail="",
        )

    def test_summary_and_exit_code_agree_fail(self) -> None:
        crafted = [self._result("a", "PASS"), self._result("b", "FAIL")]
        with mock.patch.object(runner, "run_suite", return_value=crafted):
            code = runner.main(["--stdout-only"])
        self.assertEqual(code, 1)
        passed, failed, skipped = runner._summary(crafted)
        self.assertEqual((passed, failed, skipped), (1, 1, 0))

    def test_summary_and_exit_code_agree_pass(self) -> None:
        crafted = [self._result("a", "PASS"), self._result("b", "PASS")]
        with mock.patch.object(runner, "run_suite", return_value=crafted):
            code = runner.main(["--stdout-only"])
        self.assertEqual(code, 0)
        passed, failed, skipped = runner._summary(crafted)
        self.assertEqual((passed, failed, skipped), (2, 0, 0))


class ArtifactToolingGroupTest(unittest.TestCase):
    """Batch-3 P4 / batch-4 P3: artifact_tooling group membership growth."""

    GROUP_MODULES = {
        "test_inspect_a9_artifact_v1",
        "test_check_a9_binary_format_registry_v1",
        "test_inspect_a9_artifact_manifest_v1",
        "test_a9_artifact_inspector_user_guide_v1",
        "test_validate_a9_artifact_report_contract_v1",
    }

    def test_artifact_tooling_group_has_exactly_five_members(self) -> None:
        members = [
            entry for entry in runner.ALLOWLIST
            if entry[2] == runner.GROUP_ARTIFACT_TOOLING
        ]
        self.assertEqual(len(members), 5)
        self.assertEqual({entry[0] for entry in members}, self.GROUP_MODULES)
        for entry in members:
            self.assertEqual(entry[3], runner.UT)

    def test_allowlist_has_unique_module_names(self) -> None:
        modules = [entry[0] for entry in runner.ALLOWLIST]
        self.assertEqual(len(modules), len(set(modules)))

    def test_non_artifact_entries_exclude_artifact_group(self) -> None:
        others = [
            entry for entry in runner.ALLOWLIST
            if entry[2] != runner.GROUP_ARTIFACT_TOOLING
        ]
        self.assertEqual(
            len(others),
            len(runner.ALLOWLIST) - len(self.GROUP_MODULES),
        )
        # The five new entries must be the only ones in the new group.
        self.assertEqual(
            [entry[0] for entry in others if entry[2] == runner.GROUP_ARTIFACT_TOOLING],
            [],
        )

    def test_group_filter_selects_only_the_five(self) -> None:
        filtered = [
            entry for entry in runner.ALLOWLIST
            if entry[2] == "artifact_tooling"
        ]
        self.assertEqual(len(filtered), 5)
        self.assertEqual({entry[0] for entry in filtered}, self.GROUP_MODULES)


if __name__ == "__main__":
    unittest.main()
