#!/usr/bin/env python3
"""Offline semantic and source-policy gate for portrait branch editing and licensing."""

from __future__ import annotations

import struct
import unittest
from pathlib import Path

from a9tas_recording_v1 import (
    A9G4R2_FRAME, A9G4R2_HEADER, A9G4R2_INTERVAL, decode_a9g4r2,
    decode_archive,
)


ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "A9TasAndroid/app/src/main/java/dev/a9tas/android"


def prefix(raw: bytes, frames: int) -> bytes:
    values = list(A9G4R2_HEADER.unpack_from(raw))
    old_frames, old_intervals = values[5], values[6]
    frame_start = A9G4R2_HEADER.size
    interval_start = frame_start + old_frames * A9G4R2_FRAME.size
    intervals = []
    for index in range(old_intervals):
        chunk = raw[interval_start + index * A9G4R2_INTERVAL.size:
                    interval_start + (index + 1) * A9G4R2_INTERVAL.size]
        if A9G4R2_INTERVAL.unpack(chunk)[0] < frames:
            intervals.append(chunk)
    values[5], values[6] = frames, len(intervals)
    return (A9G4R2_HEADER.pack(*values) +
            raw[frame_start:frame_start + frames * A9G4R2_FRAME.size] +
            b"".join(intervals))


def splice(first: bytes, second: bytes) -> bytes:
    a = list(A9G4R2_HEADER.unpack_from(first))
    b = list(A9G4R2_HEADER.unpack_from(second))
    assert a[1] == b[1] and a[7] == b[7]
    a_frames, a_intervals = a[5], a[6]
    b_frames, b_intervals = b[5], b[6]
    a[5], a[6] = a_frames + b_frames, a_intervals + b_intervals
    out_frames = bytearray(first[A9G4R2_HEADER.size:
                                 A9G4R2_HEADER.size + a_frames * A9G4R2_FRAME.size])
    b_frame_start = A9G4R2_HEADER.size
    for index in range(b_frames):
        frame = bytearray(second[b_frame_start + index * A9G4R2_FRAME.size:
                                 b_frame_start + (index + 1) * A9G4R2_FRAME.size])
        tick = a_frames + index
        struct.pack_into("<Q", frame, 0, tick)
        struct.pack_into("<Q", frame, 8, tick * a[7] * 1000)
        out_frames += frame
    a_interval_start = A9G4R2_HEADER.size + a_frames * A9G4R2_FRAME.size
    out_intervals = bytearray(first[a_interval_start:])
    b_interval_start = A9G4R2_HEADER.size + b_frames * A9G4R2_FRAME.size
    for index in range(b_intervals):
        interval = bytearray(second[b_interval_start + index * A9G4R2_INTERVAL.size:
                                    b_interval_start + (index + 1) * A9G4R2_INTERVAL.size])
        struct.pack_into("<Q", interval, 0,
                         struct.unpack_from("<Q", interval)[0] + a_frames)
        out_intervals += interval
    return A9G4R2_HEADER.pack(*a) + out_frames + out_intervals


class AndroidBranchLicenseTest(unittest.TestCase):
    def test_splice_model_preserves_canonical_stream(self) -> None:
        archive = decode_archive((ROOT / "A9TasAndroid/app/src/main/assets/selftest/"
                                  "a9tas1-synthetic.a9tas").read_bytes())
        first = prefix(archive.recording, 2)
        combined = splice(first, archive.recording)
        summary = decode_a9g4r2(combined)
        self.assertEqual(summary.frame_count, 5)
        self.assertEqual(summary.interval_count, 5)
        self.assertEqual(summary.fixed_delta_us, archive.source.fixed_delta_us)

    def test_full_saved_prefix_can_continue_after_its_last_tick(self) -> None:
        archive = decode_archive((ROOT / "A9TasAndroid/app/src/main/assets/selftest/"
                                  "a9tas1-synthetic.a9tas").read_bytes())
        full_prefix = prefix(archive.recording, archive.source.frame_count)
        combined = splice(full_prefix, archive.recording)
        summary = decode_a9g4r2(combined)
        self.assertEqual(summary.frame_count, archive.source.frame_count * 2)
        self.assertEqual(summary.interval_count, archive.source.interval_count * 2)

    def test_java_editor_matches_model_invariants(self) -> None:
        source = (JAVA / "A9TasBranchEditor.java").read_text("utf-8")
        for token in ("edited.putInt(24, totalFrames)",
                      "edited.putInt(28, totalIntervals)",
                      "view.putLong(0, tick)", "view.putLong(8, Math.multiplyExact",
                      "prefixSummary.frameCount", "A9TasArchive.inspectSource(pending)",
                      "materializeReplaySource", "MAX_FRAMES = 7200"):
            self.assertIn(token, source)

    def test_branch_adopts_one_native_timeline(self) -> None:
        service = (JAVA / "TasForegroundService.java").read_text("utf-8")
        orchestrator = (JAVA / "SessionOrchestrator.java").read_text("utf-8")
        # The production entry now carries continuous/recovery/foreground
        # context through overloads.  Anchor the policy gate at the full
        # implementation rather than the removed, parameterless wrapper.
        method = service[service.index(
                "private void branchRecord(boolean continuous, boolean allowCleanRepair,"):
                         service.index("private void setState(")]
        self.assertLess(method.index("replaySelected"),
                        method.index("recordFromPausedReplay"))
        self.assertLess(method.index("recordFromPausedReplay"),
                        method.index("A9TasBranchEditor.adoptContinuous"))
        self.assertNotIn("A9TasBranchEditor.splice", method)
        self.assertIn('putBoolean("replay_pause_at_target", true)', method)
        self.assertIn("Keep the old archive immutable", method)
        for token in ("ACTION_CHECKPOINT_BRANCH", "requestCheckpointBranch",
                      "branchAfterCleanWaitingCancellation",
                      'target >= base.summary.frameCount'):
            self.assertIn(token, service)
        for token in ("waitIndefinitelyAtTickZero", "onHandoffArmed(-1)",
                      "automaticResume"):
            self.assertIn(token, orchestrator)
        self.assertIn("pausedReplayHandoff && resumePausedRace && observer != null",
                      orchestrator)
        self.assertIn('"branch".equals(kind) && ticks > 0', service)
        self.assertIn('"BRANCH_ARMED_PAUSED".equals', service)

    def test_portrait_and_signed_license_boundaries(self) -> None:
        manifest = (ROOT / "A9TasAndroid/app/src/main/AndroidManifest.xml").read_text("utf-8")
        manager = (JAVA / "LicenseManager.java").read_text("utf-8")
        service = (JAVA / "TasForegroundService.java").read_text("utf-8")
        issuer = (ROOT / "issue-a9tas-license-v1.ps1").read_text("utf-8")
        self.assertIn('android:screenOrientation="portrait"', manifest)
        for token in ("SHA256withECDSA", "PUBLIC_KEY_BASE64", "deviceCode(Context",
                      "MAX_VALIDITY_SECONDS", "检测到系统时间回拨"):
            self.assertIn(token, manager)
        self.assertIn('licenseInput.setText("")',
                      (JAVA / "MainActivity.java").read_text("utf-8"))
        self.assertIn('android:inputType="textPassword"',
                      (ROOT / "A9TasAndroid/app/src/main/res/layout/activity_main.xml")
                      .read_text("utf-8"))
        self.assertNotIn("BEGIN PRIVATE KEY", manager)
        self.assertNotIn("ImportPkcs8PrivateKey", manager)
        self.assertIn("ImportPkcs8PrivateKey", issuer)
        self.assertIn("SignData", issuer)
        self.assertLess(service.index("if (ACTION_RESTORE.equals(action))"),
                        service.index("LicenseManager.requireValid(this)"))
        self.assertIn("LicenseManager.requireValid(this)", service)
        self.assertEqual(list(ROOT.rglob("*.pk8")), [])


if __name__ == "__main__":
    unittest.main()
