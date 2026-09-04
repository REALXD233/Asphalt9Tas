#!/usr/bin/env python3
"""Offline policy checks for the one-call passive Nitro recording smoke gate."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "natural_action_recording_nitro_smoke_v1.cpp"
RUNNER = ROOT / "run-natural-action-recording-nitro-smoke-v1.ps1"
HOST = ROOT / "src" / "natural_action_recording_host_v1.h"


class NitroRecordingSmokePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.runner = RUNNER.read_text(encoding="utf-8")
        cls.host = HOST.read_text(encoding="utf-8")

    def test_finish_resets_completed_receipt_for_same_process_reuse(self) -> None:
        for token in (
            "Reset the mapped",
            "const protocol::Evidence cleared_evidence = MakeEvidence();",
            "cleared_counts(protocol::kMaximumFrames)",
            "evidence_cleared && counts_cleared && control_cleared",
        ):
            self.assertIn(token, self.host)
        restore = self.host.index("result.service_restored =")
        clear = self.host.index("const protocol::Evidence cleared_evidence")
        self.assertLess(restore, clear)

    def test_wrapper_is_installed_and_removed_only_with_a_stable_freeze(self) -> None:
        install_freeze = self.source.index("FreezeStable(pid, &threads")
        stage = self.source.index("host::Stage(mem, &runtime)")
        arm = self.source.index("host::ArmFirstFrame(mem, &runtime)")
        detach = self.source.index("DetachAll(&threads)", arm)
        poll = self.source.index("runtime.payload.evidence", detach)
        cleanup_freeze = self.source.index("FreezeStable(pid, &threads", poll)
        finish = self.source.index("host::Finish(mem, &runtime, &result)", cleanup_freeze)
        self.assertLess(install_freeze, stage)
        self.assertLess(stage, arm)
        self.assertLess(arm, detach)
        self.assertLess(detach, poll)
        self.assertLess(poll, cleanup_freeze)
        self.assertLess(cleanup_freeze, finish)

    def test_action_window_has_no_hardware_breakpoint_or_long_ptrace_loop(self) -> None:
        for token in (
            "hardware_breakpoints=0",
            "count == 1",
            "evidence.wrapper_entries == 1",
            "evidence.original_calls == 1",
            "evidence.clean_returns == 1",
            "result.count_sum == 1",
        ):
            self.assertIn(token, self.source)
        action_window = self.source[
            self.source.index("detached_for_action = true") :
            self.source.index("unlink(argv[5])")
        ]
        self.assertNotIn("ptrace(", action_window)
        self.assertNotIn("PTRACE_", action_window)
        self.assertNotIn("ProgramStoppedThread", action_window)

    def test_runner_is_offline_by_default_and_manual_input_is_exact(self) -> None:
        for token in (
            '[string]$Mode = "OfflineValidate"',
            "deployed=0 device_access=0 hardware_breakpoints=0",
            "AcknowledgeExactlyOneManualSpace",
            "现在只按一次空格，不要双击，也不要按其他键。",
            "NITRO_RECORDING_SMOKE_DONE complete=1",
            "safe_rollback",
            "TracerPid=0 hardware_breakpoints=0",
        ):
            self.assertIn(token, self.runner)
        offline = self.runner.index('if ($Mode -eq "OfflineValidate")')
        adb_check = self.runner.index("Test-Path -LiteralPath $AdbPath")
        self.assertLess(offline, adb_check)

    def test_runner_releases_only_after_exact_frozen_ready_proof(self) -> None:
        ready = self.runner.index("Smoke READY proof mismatch")
        esc = self.runner.index("$esc = Start-Process", ready)
        release = self.runner.index("rm -f $readyPath", esc)
        input_prompt = self.runner.index("NITRO_RECORDING_SMOKE_INPUT_WINDOW_OPEN", release)
        self.assertLess(ready, esc)
        self.assertLess(esc, release)
        self.assertLess(release, input_prompt)
        self.assertIn("service_vptr_swaps=1 hardware_breakpoints=0", self.runner)


if __name__ == "__main__":
    unittest.main()
