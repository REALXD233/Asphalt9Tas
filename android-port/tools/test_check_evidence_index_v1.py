#!/usr/bin/env python3
"""Offline unit tests for check_evidence_index_v1.py.

Uses a temporary workspace with minimal Markdown fixtures and covers:
1. live result + failed-closed verdict -> FAIL_CLOSED;
2. live pass -> LIVE_PASS;
3. PT_NB0_* -> RETIRED;
4. FC1_GUARDED_RUNNER_* -> BUILD_ONLY;
5. ratios, offsets, device paths and command lines never enter missing refs;
6. basenames resolve uniquely from tools/ and src/;
7. ``path:line-range`` references resolve to the file;
8. hash-before-path and path-before-hash bindings are both verified;
9. a hash without a path stays unbound;
10. generated reports are never indexed.
"""

from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import check_evidence_index_v1 as b1

H1 = "a" * 64
H2 = "b" * 64


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def content_hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class ClassificationTest(unittest.TestCase):
    def _record(self, workspace: Path, name: str, text: str):
        evidence = workspace / "android-port" / "evidence"
        write(evidence / name, text)
        index = b1.build_index(workspace)
        return next(r for r in index.records if r.file == name)

    def test_live_result_with_failed_closed_verdict_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            record = self._record(
                Path(tmp),
                "DUAL_THREAD_HOST_PROBE_DTA0_LIVE_RESULT_20260818.md",
                "# DTA-0 dual-thread host-only live result (2026-08-18)\n"
                "\n"
                "## Verdict\n"
                "\n"
                "DTA-0 failed closed at the one permitted host `gettid()` call.\n",
            )
        self.assertEqual(record.status, "FAIL_CLOSED")

    def test_live_pass_is_live_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            record = self._record(
                Path(tmp),
                "GAME_ACTION_SUBMISSION_AFFINITY_LIVE_PASS_20260818.md",
                "# Game action submission affinity — live pass (2026-08-18)\n"
                "\n"
                "## Verdict\n"
                "\n"
                "PASS. One manual touchscreen Nitro-button action produced a "
                "complete lifecycle.\n",
            )
        self.assertEqual(record.status, "LIVE_PASS")

    def test_pt_nb0_is_retired(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            record = self._record(
                Path(tmp),
                "PT_NB0_PREPARE_FRESH_PROCESS_PASS_20260818.md",
                "# PT-NB0 fresh-process preparation PASS\n"
                "\n"
                "## Verdict\n"
                "\n"
                "prepared a fresh game process successfully\n",
            )
        self.assertEqual(record.status, "RETIRED")
        self.assertEqual(record.status_source, "policy")

    def test_fc1_guarded_runner_is_build_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            record = self._record(
                Path(tmp),
                "FC1_GUARDED_RUNNER_BUILD_ONLY_20260818.md",
                "# FC-1 guarded runner — build-only PASS\n"
                "\n"
                "## Verdict\n"
                "\n"
                "The FC-1 runner is implemented and its default mode is "
                "genuinely offline.\n",
            )
        self.assertEqual(record.status, "BUILD_ONLY")


class ReferenceFilterTest(unittest.TestCase):
    def test_ratios_offsets_device_paths_commands_not_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            workspace = Path(tmp)
            evidence = workspace / "android-port" / "evidence"
            name = "FILTER_FIXTURE_20260818.md"
            write(
                evidence / name,
                "# Filter fixture\n"
                "\n"
                "`7/7` `14/14` `276/276` `+0.0/-0.0` `0/0/1` "
                "`0x4D10E94/+0x4` `+0x1360/+0x1368` "
                "`/proc/PID/mem` `/system/lib64/libhoudini.so` "
                "`/data/local/tmp/a9tas_action_window_start_<pid>` "
                "`python android-port/tools/check_evidence_index_v1.py` "
                "`android-port/tools/test_*.py` `SIGSEGV/0xdead0000` "
                "`A9USR3/3` `activations=1/2`\n",
            )
            index = b1.build_index(workspace)
        file_missing = [
            item for item in index.missing_references if item["file"] == name
        ]
        file_ambiguous = [
            item for item in index.ambiguous_references if item["file"] == name
        ]
        self.assertEqual(file_missing, [])
        self.assertEqual(file_ambiguous, [])

    def test_basenames_resolve_from_tools_and_src(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            workspace = Path(tmp)
            evidence = workspace / "android-port" / "evidence"
            tools = workspace / "android-port" / "tools"
            src = workspace / "android-port" / "src"
            tools.mkdir(parents=True)
            src.mkdir(parents=True)
            (tools / "parse_conditional_audit_v1.py").write_text(
                "# parser\n", encoding="utf-8"
            )
            (src / "hwbp_barrel_angular_observer_v1.cpp").write_text(
                "// observer\n", encoding="utf-8"
            )
            name = "BASENAME_FIXTURE_20260818.md"
            write(
                evidence / name,
                "# Basename fixture\n"
                "\n"
                "`parse_conditional_audit_v1.py` and "
                "`hwbp_barrel_angular_observer_v1.cpp`\n",
            )
            index = b1.build_index(workspace)
            record = next(r for r in index.records if r.file == name)
        resolved = {ref.reference: ref for ref in record.referenced_files}
        self.assertIn("parse_conditional_audit_v1.py", resolved)
        self.assertTrue(resolved["parse_conditional_audit_v1.py"].exists)
        self.assertTrue(
            resolved["parse_conditional_audit_v1.py"].resolved_path.endswith(
                "tools" + ("\\" if sys.platform == "win32" else "/") + "parse_conditional_audit_v1.py"
            )
        )
        self.assertIn("hwbp_barrel_angular_observer_v1.cpp", resolved)
        self.assertTrue(resolved["hwbp_barrel_angular_observer_v1.cpp"].exists)

    def test_path_line_range_resolves(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            workspace = Path(tmp)
            evidence = workspace / "android-port" / "evidence"
            ref_dir = workspace / "ref-alu-tas-v2"
            ref_dir.mkdir(parents=True)
            (ref_dir / "DetourFunctions.cpp").write_text(
                "// detours\n", encoding="utf-8"
            )
            name = "RANGE_FIXTURE_20260818.md"
            write(
                evidence / name,
                "# Range fixture\n"
                "\n"
                "See `ref-alu-tas-v2/DetourFunctions.cpp:1435-1464`\n",
            )
            index = b1.build_index(workspace)
            record = next(r for r in index.records if r.file == name)
        self.assertEqual(len(record.referenced_files), 1)
        ref = record.referenced_files[0]
        self.assertEqual(ref.reference, "ref-alu-tas-v2/DetourFunctions.cpp:1435-1464")
        self.assertTrue(ref.exists)
        self.assertTrue(ref.resolved_path.endswith("DetourFunctions.cpp"))


class HashBindingTest(unittest.TestCase):
    def _build(self, fixture_text: str, files: dict[str, bytes]):
        with tempfile.TemporaryDirectory() as tmp:
            workspace = Path(tmp)
            evidence = workspace / "android-port" / "evidence"
            for rel, data in files.items():
                path = workspace / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            write(evidence / "HASH_FIXTURE_20260818.md", fixture_text)
            index = b1.build_index(workspace)
            return next(r for r in index.records if r.file == "HASH_FIXTURE_20260818.md")

    def test_hash_before_path_and_path_before_hash_verified(self) -> None:
        data_a = b"content a"
        data_b = b"content b"
        hash_a = content_hash(data_a)
        hash_b = content_hash(data_b)
        fixture = (
            "# Hash fixture\n"
            "\n"
            f"`{hash_a}` `tools/file_a.py`\n"
            f"`tools/file_b.py` `{hash_b}`\n"
        )
        record = self._build(
            fixture,
            {
                "android-port/tools/file_a.py": data_a,
                "android-port/tools/file_b.py": data_b,
            },
        )
        by_path = {
            ref.reference: ref
            for ref in record.referenced_files
            if ref.reference in ("tools/file_a.py", "tools/file_b.py")
        }
        self.assertEqual(len(by_path), 2)
        self.assertTrue(by_path["tools/file_a.py"].hash_matches)
        self.assertTrue(by_path["tools/file_b.py"].hash_matches)
        declared = {
            (d.referenced_path, d.sha256) for d in record.declared_hashes
        }
        self.assertIn(("tools/file_a.py", hash_a), declared)
        self.assertIn(("tools/file_b.py", hash_b), declared)

    def test_hash_without_path_stays_unbound(self) -> None:
        record = self._build(
            "# Hash fixture\n\n`%s` has no path next to it\n" % H1,
            {},
        )
        self.assertEqual(record.declared_hashes, [])
        self.assertEqual(len(record.sha256_values), 1)
        self.assertIn(H1, record.sha256_values)

    def test_adjacent_line_manifest_binds_in_both_directions(self) -> None:
        data = b"src file"
        real_hash = content_hash(data)
        # path line then hash line (manifest style A)
        fixture_a = "# Hash fixture\n\n`tools/file_c.py`\n`%s`\n" % real_hash
        record = self._build(fixture_a, {"android-port/tools/file_c.py": data})
        ref = next(r for r in record.referenced_files if r.reference == "tools/file_c.py")
        self.assertTrue(ref.hash_matches)
        # hash line then path line (manifest style B)
        fixture_b = "# Hash fixture\n\n`%s`\n`tools/file_d.py`\n" % real_hash
        record = self._build(fixture_b, {"android-port/tools/file_d.py": data})
        ref = next(r for r in record.referenced_files if r.reference == "tools/file_d.py")
        self.assertTrue(ref.hash_matches)


class GeneratedReportsTest(unittest.TestCase):
    def test_generated_reports_not_indexed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            workspace = Path(tmp)
            evidence = workspace / "android-port" / "evidence"
            evidence.mkdir(parents=True)
            for name in (
                "EVIDENCE_INDEX_20260818.md",
                "OFFLINE_REGRESSION_SUITE_V1.md",
                "DOCUMENT_STALENESS_AUDIT_20260818.md",
                "HANDOFF_FOR_MAIN_AGENT_REVIEW_20260818.md",
            ):
                write(evidence / name, f"# {name}\n")
            write(evidence / "REAL_EVIDENCE_20260818.md", "# Real evidence\n")
            index = b1.build_index(workspace)
            names = {r.file for r in index.records}
        self.assertEqual(names, {"REAL_EVIDENCE_20260818.md"})


if __name__ == "__main__":
    unittest.main()
