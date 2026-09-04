#!/usr/bin/env python3
"""Static safety policy for the zero-write search runner."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = (ROOT / "android-port/run-authoritative-steering-search-only-v1.ps1").read_text(encoding="utf-8")


class SearchRunnerPolicyTests(unittest.TestCase):
    def test_offline_return_precedes_device_access(self) -> None:
        offline = RUNNER.index("if($OfflineValidateOnly)")
        self.assertLess(offline, RUNNER.index("& $AdbPath devices", offline))

    def test_single_escape_is_unique_gated_and_before_execution(self) -> None:
        lower = RUNNER.lower()
        self.assertEqual(lower.count("shell input"), 1)
        self.assertEqual(lower.count("keyevent 111"), 1)
        self.assertGreaterEqual(RUNNER.count("AdbEscapeResumeImmediatelyBeforeSearch"), 4)
        self.assertGreaterEqual(RUNNER.count("AcknowledgeSingleEscapeResumeInput"), 3)
        self.assertLess(RUNNER.index("shell input keyevent 111"),
                        RUNNER.index("'$remoteBinary $gamePid"))

    def test_binary_policy_and_post_detach_checks_are_mandatory(self) -> None:
        self.assertIn("pwrite_imports=0", RUNNER)
        self.assertIn("test_authoritative_steering_search_only_policy_v1.py", RUNNER)
        self.assertGreaterEqual(RUNNER.count("Get-Tracer"), 4)
        self.assertIn("validate_authoritative_steering_search_only_v1.py", RUNNER)


if __name__ == "__main__":
    unittest.main()
