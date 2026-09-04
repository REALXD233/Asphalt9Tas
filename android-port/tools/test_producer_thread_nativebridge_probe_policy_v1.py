#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = (ROOT / "src" / "producer_thread_nativebridge_probe_controller_v1.cpp").read_text(
    encoding="utf-8"
)
BOOTSTRAP = (ROOT / "src" / "bootstrap_producer_thread_probe_v1_build.cpp").read_text(
    encoding="utf-8"
)
PAYLOAD = (ROOT / "src" / "payload_same_thread_probe_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-producer-thread-nativebridge-probe-v1.ps1").read_text(
    encoding="utf-8"
)


class ProducerThreadNativeBridgeProbePolicyTests(unittest.TestCase):
    def test_gate_requires_explicit_ack_and_zero_rip_bias(self) -> None:
        self.assertIn(
            "I_ACCEPT_PRODUCER_THREAD_NATIVEBRIDGE_GETTID_PROBE_V1",
            CONTROLLER,
        )
        self.assertIn("bias_value != 0", CONTROLLER)
        self.assertIn("RcSetRipBias(0);", CONTROLLER)

    def test_no_host_calibration_or_host_gettid_call(self) -> None:
        self.assertNotIn("CalibrateRipBias", CONTROLLER)
        self.assertNotIn('RemoteSymbolByName(pid, "gettid")', CONTROLLER)
        self.assertIn("host_calls=0", CONTROLLER)

    def test_only_one_nativebridge_call_site_exists(self) -> None:
        self.assertEqual(CONTROLLER.count("RemoteCallSessionCall("), 1)
        self.assertIn("&session, trampoline, zero_args", CONTROLLER)
        self.assertIn("returned_tid == static_cast<std::uint64_t>(producer_tid)", CONTROLLER)
        self.assertIn("producer->stopped = call_report.stop_confirmed", CONTROLLER)

    def test_requires_stable_fixed_delta_producer(self) -> None:
        self.assertIn("kRequiredProducerHits = 2", CONTROLLER)
        self.assertIn("producer affinity conflict", CONTROLLER)
        self.assertIn("delta_after == delta_before", CONTROLLER)
        self.assertIn('name.rfind("Thread-", 0) == 0', CONTROLLER)

    def test_build_and_main_object_are_locally_verified(self) -> None:
        self.assertIn("VerifySupportedBuildV1(pid, base)", CONTROLLER)
        self.assertIn("ValidateMainObjectV1(pid, base, main_object)", CONTROLLER)
        self.assertIn("ResolveUniqueMainObjectV1(pid, base, &main_object)", CONTROLLER)
        self.assertIn("candidates.size() != 1", CONTROLLER)
        self.assertIn("main_object + kAccumulatorOffset", CONTROLLER)
        self.assertIn("kBuildSignatures", CONTROLLER)

    def test_no_game_action_or_gameplay_write_route(self) -> None:
        forbidden = (
            "0x367B414",
            "dispatch_action",
            "nitro_activation",
            "WriteProcessMemory",
            "WriteExactVerified",
        )
        for token in forbidden:
            self.assertNotIn(token, CONTROLLER)
        self.assertIn("game_calls=0", CONTROLLER)
        self.assertIn("gameplay_writes=0", CONTROLLER)

    def test_guest_payload_is_gettid_only(self) -> None:
        self.assertIn("syscall(__NR_gettid)", PAYLOAD)
        self.assertNotIn("libAsphalt9", PAYLOAD)
        self.assertNotIn("dlopen", PAYLOAD)

    def test_isolated_early_load_identity(self) -> None:
        self.assertIn("#define A9TAS_ENABLE_SAME_THREAD_PROBE 1", BOOTSTRAP)
        self.assertIn("liba9tas_producer_thread_probe_v1.so", BOOTSTRAP)
        self.assertIn("liba9tas_bootstrap_producer_thread_probe_v1.so", BUILD)
        self.assertIn("PT_NB0_BUILD_ONLY", BUILD)


if __name__ == "__main__":
    unittest.main()
