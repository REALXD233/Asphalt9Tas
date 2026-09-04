#!/usr/bin/env python3
"""Offline safety-policy tests for the guarded conditional live runner."""

from __future__ import annotations

import hashlib
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from native_physics_recording_v1 import NativePhysicsFrameV1, encode_recording


ANDROID_PORT = Path(__file__).resolve().parents[1]
RUNNER = ANDROID_PORT / "run-hwbp-conditional-executor-v1.ps1"
BINARY = ANDROID_PORT / "build" / "staging" / "a9tas_hwbp_conditional_executor_v1"
EXPECTED_BINARY_HASH = (
    "57c74e137144f6d8a65d94caf35f2fc0d2a9ccf6520d6c77d5412399ba1e3660"
)


def recording(frame_count: int) -> bytes:
    transform = struct.pack("<16f", *[float(index) for index in range(16)])
    linear = struct.pack("<3f", 1.0, 2.0, 3.0)
    return encode_recording(
        NativePhysicsFrameV1(index, 1000 + index, transform, linear)
        for index in range(frame_count)
    )


def offline_validate(blob: bytes) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "short.a9nps1"
        path.write_bytes(blob)
        digest = hashlib.sha256(blob).hexdigest()
        return subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(RUNNER),
                "-RecordingPath",
                str(path),
                "-ExpectedRecordingHash",
                digest,
                "-OfflineValidateOnly",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )


class ConditionalRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = RUNNER.read_text(encoding="utf-8")

    def test_pins_the_exact_reviewed_binary(self) -> None:
        self.assertEqual(hashlib.sha256(BINARY.read_bytes()).hexdigest(), EXPECTED_BINARY_HASH)
        self.assertIn(EXPECTED_BINARY_HASH, self.source)

    def test_runtime_acknowledgements_precede_any_adb_discovery(self) -> None:
        adb_start = self.source.index("$deviceLine =")
        for guard in (
            "if (-not $Gate2Validated)",
            "if (-not $SameBytesValidated)",
            "if (-not $AcknowledgeRacePausedThenResume)",
            "if (-not $AcknowledgeShortDifferentValueWrite)",
            "if (-not $AcknowledgeRollbackIsBestEffort)",
        ):
            self.assertLess(self.source.index(guard), adb_start)

    def test_runner_hard_caps_and_requires_a_real_correction(self) -> None:
        self.assertIn("$maximumLiveFrames = 10", self.source)
        self.assertIn("--minimum-corrected 1", self.source)
        self.assertIn("--maximum-frames $maximumLiveFrames", self.source)

    def test_offline_validation_accepts_three_frames_without_adb(self) -> None:
        result = offline_validate(recording(3))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CONDITIONAL_RUNNER_OFFLINE_VALIDATION_OK", result.stdout)
        self.assertIn("frames=3", result.stdout)

    def test_offline_validation_rejects_more_than_ten_frames(self) -> None:
        result = offline_validate(recording(11))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must contain 1..10 frames", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
