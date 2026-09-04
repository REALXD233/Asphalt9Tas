#!/usr/bin/env python3
"""Static safety and scope policy for resident unified start-line replay."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/hwbp_startline_unified_replay_v1.cpp").read_text(encoding="utf-8")
CORE = (ROOT / "android-port/src/hwbp_unified_tick_executor_v1.cpp").read_text(encoding="utf-8")
BUILD = (ROOT / "android-port/build-startline-unified-replay-v1.ps1").read_text(encoding="utf-8")


class StartlineUnifiedReplayPolicyTests(unittest.TestCase):
    def test_prearm_is_read_only_before_wrapped_executor(self) -> None:
        readonly = SOURCE.index("open(mem_path, O_RDONLY")
        ready = SOURCE.index("STARTLINE_UNIFIED_READY_NO_ATTACH", readonly)
        resume = SOURCE.index("STARTLINE_UNIFIED_RESUME_OBSERVED", ready)
        wrapped = SOURCE.index("a9tas_unified_replay_main_v1(argc, argv)", resume)
        self.assertLess(readonly, ready)
        self.assertLess(ready, resume)
        self.assertLess(resume, wrapped)

    def test_unified_scope_enables_brake_and_keeps_nitro_disabled(self) -> None:
        self.assertIn("#define A9TAS_UNIFIED_BRAKE_V1 1", SOURCE)
        self.assertNotIn("A9TAS_UNIFIED_NITRO_RPC_V1", SOURCE)
        self.assertIn("I_ACCEPT_STARTLINE_UNIFIED_STEERING_BRAKE_PHYSICS_V1", SOURCE)
        self.assertIn("kSkipTransformForced", CORE)
        self.assertIn("frame.transform_bits", CORE)
        self.assertIn("frame.linear_velocity_bits", CORE)

    def test_host_acknowledges_resume_before_delta_can_attach(self) -> None:
        ready = SOURCE.index("host_resume_gate=marker_removal")
        acknowledged = SOURCE.index("host_resume_acknowledged", ready)
        observe = SOURCE.index("ObserveBeforeAttach", acknowledged)
        self.assertLess(ready, acknowledged)
        self.assertLess(acknowledged, observe)

    def test_default_core_entry_remains_source_compatible(self) -> None:
        self.assertIn("A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME", CORE)
        self.assertIn("A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME(int argc, char** argv)", CORE)

    def test_build_links_prearm_protocol_and_has_no_live_command(self) -> None:
        self.assertIn("startline_prearm_protocol_v1.cpp", BUILD)
        for forbidden in ("adb", "keyevent", "su -c"):
            self.assertNotIn(forbidden, BUILD.lower())


if __name__ == "__main__":
    unittest.main()
