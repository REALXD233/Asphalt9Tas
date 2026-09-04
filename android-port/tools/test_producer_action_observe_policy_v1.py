#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHARED = (ROOT / "src" / "payload_nitro_rpc_v1.cpp").read_text(encoding="utf-8")
PAYLOAD = (ROOT / "src" / "payload_producer_action_observe_v1.cpp").read_text(
    encoding="utf-8"
)
BOOTSTRAP = (ROOT / "src" / "bootstrap_producer_action_observe_v1_build.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-producer-action-observe-v1.ps1").read_text(encoding="utf-8")


class ProducerActionObservePolicyTests(unittest.TestCase):
    def test_shared_server_default_remains_enabled(self) -> None:
        self.assertIn("#define A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD 1", SHARED)
        self.assertIn("#if A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD", SHARED)

    def test_observe_payload_has_no_worker_and_no_execute(self) -> None:
        self.assertIn("#define A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD 0", PAYLOAD)
        self.assertIn("#define A9TAS_NITRO_RPC_ENABLE_STATUS_WRITES 0", PAYLOAD)
        self.assertIn("#define A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE 0", PAYLOAD)
        self.assertNotIn("pthread_create", PAYLOAD)
        self.assertNotIn("socket(", PAYLOAD)

    def test_request_is_compile_time_observe_only(self) -> None:
        self.assertIn("request.activations = 0;", PAYLOAD)
        self.assertIn("request.flags = a9tas::nitro_rpc_v1::kFlagObserveOnly;", PAYLOAD)
        self.assertNotIn("kFlagExternalTickStopped", PAYLOAD)

    def test_return_binds_result_to_actual_tid(self) -> None:
        self.assertIn("syscall(__NR_gettid)", PAYLOAD)
        self.assertIn("static_cast<std::uint64_t>(tid) << 32", PAYLOAD)
        self.assertIn("result_bits", PAYLOAD)

    def test_bridge_uses_one_long_argument(self) -> None:
        self.assertIn('A9TAS_SAME_THREAD_PROBE_SHORTY "JJ"', BOOTSTRAP)
        self.assertIn("a9tas_producer_action_observe_v1", BOOTSTRAP)

    def test_build_demands_legacy_binary_parity(self) -> None:
        self.assertIn("LegacyGameActionRpcSha256", BUILD)
        self.assertIn("legacy game-action RPC binary parity failed", BUILD)
        self.assertIn("forbidden server import", BUILD)
        self.assertIn("PT_NB1_BUILD_ONLY", BUILD)


if __name__ == "__main__":
    unittest.main()
