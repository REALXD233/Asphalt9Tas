#!/usr/bin/env python3
"""Offline tests for the non-technical user guide (P3).

Reads only the Markdown guide, verifies required terms, forbidden claims,
example-command script existence and that the guide is byte-identical
before/after the test.  No device access.
"""

from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path

GUIDE = Path(__file__).resolve().parent.parent / "docs" / "A9_ARTIFACT_INSPECTOR_USER_GUIDE_20260818.md"
TOOLS = Path(__file__).resolve().parent

REQUIRED_TERMS = (
    "inspect_a9_artifact_v1.py",
    "--companion",
    "inspect_a9_artifact_manifest_v1.py",
    "退出码",
    "STRUCTURE_ONLY",
    "CAPTURE_CROSS_BOUND",
    "capture_validated",
    "不会自动寻找",
    "只读",
    "不等于实机回放成功",
    "完整比赛确定性",
    "路径不存在",
    "wrong version",
    "companion",
    "FC1",
    "--pid",
    "--base",
    "--list-formats",
    "A9_FORMAT_LIST_V1",
)

FORBIDDEN_CLAIMS = (
    "离线检查通过 = TAS 成功",
    "离线检查通过=TAS 成功",
    "绝不会崩溃",
    "自动修复",
    "自动重试",
    "游戏世界状态已回滚",
    "LIVE PASS",
    "live pass",
    "已回滚",
)

SCRIPT_PATTERNS = (
    r"android-port/tools/[A-Za-z0-9_]+\.py",
    r"(?<![\w./])tools/[A-Za-z0-9_]+\.py",
)


def guide_text() -> str:
    return GUIDE.read_text(encoding="utf-8")


class UserGuideTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.before_sha = hashlib.sha256(GUIDE.read_bytes()).hexdigest()
        cls.text = guide_text()

    def test_required_terms_present(self) -> None:
        for term in REQUIRED_TERMS:
            with self.subTest(term=term):
                self.assertIn(term, self.text)

    def test_forbidden_claims_absent(self) -> None:
        lowered = self.text.lower()
        for claim in FORBIDDEN_CLAIMS:
            with self.subTest(claim=claim):
                self.assertNotIn(claim.lower(), lowered)

    def test_example_scripts_exist(self) -> None:
        mentioned: set[str] = set()
        for pattern in SCRIPT_PATTERNS:
            for match in re.finditer(pattern, self.text):
                mentioned.add(match.group(0))
        self.assertTrue(mentioned, "guide must reference at least one script")
        for script in mentioned:
            with self.subTest(script=script):
                relative = script.removeprefix("android-port/")
                self.assertTrue((TOOLS.parent / relative).is_file(), f"missing {script}")

    def test_structure_only_vs_cross_bound_distinguished(self) -> None:
        self.assertIn("STRUCTURE_ONLY", self.text)
        self.assertIn("CAPTURE_CROSS_BOUND", self.text)
        self.assertIn("NOT_PROVIDED", self.text)

    def test_guide_sha256_unchanged(self) -> None:
        self.assertEqual(hashlib.sha256(GUIDE.read_bytes()).hexdigest(), self.before_sha)

    def test_read_only_claims_present(self) -> None:
        self.assertIn("只读", self.text)
        self.assertIn("不会修改任何录像", self.text)


if __name__ == "__main__":
    unittest.main()
