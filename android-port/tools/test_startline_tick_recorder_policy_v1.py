#!/usr/bin/env python3
"""Fail-closed source policy for the start-line recorder wrapper."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/hwbp_startline_tick_recorder_v1.cpp").read_text(encoding="utf-8")


class StartlineTickRecorderPolicyTests(unittest.TestCase):
    def test_prearm_is_read_only_and_precedes_legacy_recorder(self) -> None:
        prearm = SOURCE.index("bool PrearmUntilResume")
        readonly = SOURCE.index("O_RDONLY", prearm)
        ready = SOURCE.index("STARTLINE_READY_NO_ATTACH", readonly)
        resume = SOURCE.index("STARTLINE_RESUME_OBSERVED", ready)
        legacy = SOURCE.index("a9tas_synchronized_tick_recorder_main_v1(argc, argv)", resume)
        self.assertLess(prearm, readonly)
        self.assertLess(readonly, ready)
        self.assertLess(ready, resume)
        self.assertLess(resume, legacy)

    def test_ready_marker_claims_zero_attach_and_zero_writes(self) -> None:
        for token in (
            "READY_NO_ATTACH_V1", "target_threads_attached=0",
            "game_writes=0", "paused_zero_samples",
            "host_resume_gate=marker_removal",
            "host resume acknowledgement missing; no attach",
            "attach_permission=1", "attach_attempts=0",
        ):
            self.assertIn(token, SOURCE)

    def test_prearm_has_no_target_write_primitive(self) -> None:
        prearm = SOURCE[SOURCE.index("bool PrearmUntilResume"):SOURCE.index("}  // namespace")]
        for forbidden in ("pwrite", "process_vm_writev", "ptrace("):
            self.assertNotIn(forbidden, prearm)
        self.assertIn("open(mem_path, O_RDONLY", prearm)

    def test_distinct_runtime_acknowledgement(self) -> None:
        self.assertIn("I_ACCEPT_STARTLINE_PREARM_FIXED_DELTA_BRAKE_CAPTURE_V1", SOURCE)
        self.assertIn("argv[8] = const_cast<char*>(kSyncAcknowledgement)", SOURCE)


if __name__ == "__main__":
    unittest.main()
