#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "payload_nitro_rpc_v1.cpp").read_text(
    encoding="utf-8"
)
WRAPPER = (ROOT / "src" / "payload_game_action_rpc_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-game-action-rpc-v1.ps1").read_text(encoding="utf-8")


class GameActionRpcBuildPolicyTests(unittest.TestCase):
    def test_isolated_wrapper_selects_route_but_disables_execute(self) -> None:
        self.assertIn("#define A9TAS_GAME_ACTION_RPC_V1 1", WRAPPER)
        self.assertIn("#define A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE 0", WRAPPER)

    def test_shared_source_defaults_remain_on_legacy_route(self) -> None:
        self.assertIn("#define A9TAS_GAME_ACTION_RPC_V1 0", SOURCE)
        self.assertIn("#define A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE 0", SOURCE)

    def test_observe_artifact_has_distinct_transport_identity(self) -> None:
        self.assertIn('kProtocolName = "game-action-rpc-v1"', SOURCE)
        self.assertIn('"a9tas-game-action-rpc-v1.sock"', SOURCE)
        self.assertIn('"a9tas-game-action-rpc-v1.status"', SOURCE)

    def test_nonzero_request_fails_before_sequence_consumption(self) -> None:
        gate = "if (execute) return Result::kBadRequest;"
        sequence = "const std::uint64_t last ="
        self.assertIn(gate, SOURCE)
        self.assertLess(SOURCE.index(gate), SOURCE.index(sequence))

    def test_action_call_is_behind_second_compile_time_gate(self) -> None:
        guarded = (
            "#if A9TAS_GAME_ACTION_RPC_V1 && "
            "A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE"
        )
        self.assertIn(guarded, SOURCE)
        self.assertIn("dispatch_action(reinterpret_cast<void*>(owner));", SOURCE)
        self.assertIn(
            "#elif A9TAS_GAME_ACTION_RPC_V1\n"
            "        // Defense in depth",
            SOURCE,
        )

    def test_exact_route_identity_and_vector_shape_are_checked(self) -> None:
        self.assertIn("kGameActionDispatchRva = 0x367B414", SOURCE)
        self.assertIn("kCommandQueueOffset = 0x1360", SOURCE)
        self.assertIn("kDirectModeOffset = 0x1378", SOURCE)
        self.assertIn("queue_begin <= queue_end", SOURCE)
        self.assertIn("queue_end <= queue_capacity", SOURCE)

    def test_build_is_separate_and_checks_aarch64(self) -> None:
        self.assertIn("liba9tas_game_action_rpc_v1.so", BUILD)
        self.assertIn('if ($header -notmatch "AArch64")', BUILD)
        self.assertIn("execute_compile_gate=0", BUILD)


if __name__ == "__main__":
    unittest.main()
