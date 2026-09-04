#!/usr/bin/env python3
"""Offline policy for the lifecycle source-to-replay artifact bridge."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "prepare-lifecycle-replay-artifacts-v1.ps1"


class PrepareLifecycleReplayArtifactsPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_default_is_zero_write_and_zero_device_access(self) -> None:
        self.assertIn('[string]$Mode = "ValidateTooling"', self.text)
        self.assertIn("writes=0 device_access=0", self.text)
        offline = self.text.index('if ($Mode -eq "ValidateTooling")')
        first_output_write = self.text.index("[IO.File]::WriteAllText")
        self.assertLess(offline, first_output_write)
        self.assertNotIn("adb", self.text.lower())

    def test_preparation_requires_lifecycle_proof_and_exact_shape(self) -> None:
        for needle in (
            "lifecycle_source_recording_v1.py",
            "A9USR5/A9UTK1 lifecycle source validation failed",
            "make_final_writer_target_blob_v1.py",
            "96 + 360 * 144",
            "128 + 360 * 80",
            "Generated A9FWT1 is not SHA-bound to A9UTK1",
        ):
            self.assertIn(needle, self.text)

    def test_outputs_are_non_overwriting_and_manifest_is_hash_complete(self) -> None:
        self.assertIn("Refusing to overwrite existing output", self.text)
        for needle in (
            'semantic_origin = "authoritative_race_lifecycle_2_to_3"',
            "recording_sha256 = $recordingHash",
            "source_report_sha256 = $sourceReportHash",
            "target_sha256 = $targetHash",
            "fixed_interval_us = 16667",
        ):
            self.assertIn(needle, self.text)


if __name__ == "__main__":
    unittest.main()
