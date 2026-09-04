#!/usr/bin/env python3
"""Offline policy and report tests for the pre-physics nitro observe gate."""

from __future__ import annotations

import pathlib
import unittest

from parse_unified_executor_report_v2 import _HEADER as _HEADER_V5
from parse_unified_executor_report_v5 import _FRAME as _FRAME_V5
from parse_unified_nitro_observe_report_v7 import (
    NITRO_RPC_OBSERVED,
    NITRO_RPC_USED,
    _FRAME,
    _HEADER,
    _RESPONSE,
    decode_report,
)
from unified_tick_recording_v1 import SKIP_NITRO, decode_recording


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp").read_text(
    encoding="utf-8"
)
CLIENT = (ROOT / "src" / "unified_nitro_rpc_client_v1.h").read_text(
    encoding="utf-8"
)
PAYLOAD = (ROOT / "src" / "payload_nitro_rpc_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (
    ROOT / "build-hwbp-unified-nitro-observe-gate-v1.ps1"
).read_text(encoding="utf-8")
RUNNER = (ROOT / "run-gate11-unified-nitro-observe-v1.ps1").read_text(
    encoding="utf-8"
)


class UnifiedNitroObserveGateTests(unittest.TestCase):
    @staticmethod
    def synthetic_report() -> bytes:
        original = (
            ROOT
            / "evidence"
            / "a9tas_gate7_steering_report_20260817_182513_762.a9uer5"
        ).read_bytes()
        old_header = list(_HEADER_V5.unpack_from(original))
        old_frame = list(_FRAME_V5.unpack_from(original, _HEADER_V5.size))
        old_header[0], old_header[1] = b"A9UER7\0\0", 7
        old_header[2], old_header[3] = _HEADER.size, _FRAME.size
        old_header[4] |= NITRO_RPC_USED
        new_header = old_header + [1, 0, 0]
        state = (0, 0, b"\x01\x01\x01\x01\x01", 0, 0, 0)
        response = _RESPONSE.pack(
            b"A9NRS1\0\0",
            1,
            96,
            123456,
            0,
            0,
            old_header[10],
            old_header[12],
            0x700000001000,
            old_header[10] + 0x3674E50,
            *state,
            *state,
        )
        old_frame[3] |= NITRO_RPC_OBSERVED
        new_frame = old_frame[:24] + [old_frame[6], 0, 0, response] + old_frame[24:]
        return _HEADER.pack(*new_header) + _FRAME.pack(*new_frame)

    def test_separate_build_and_observe_only_packet(self) -> None:
        self.assertIn("A9TAS_UNIFIED_NITRO_RPC_V1", BUILD)
        self.assertIn("A9TAS_UNIFIED_NITRO_OBSERVE_GATE_V1", BUILD)
        _, frames = decode_recording(
            (
                ROOT
                / "evidence"
                / "a9tas_gate6_phase_only_1f_draft_20260817.a9utk1"
            ).read_bytes()
        )
        self.assertEqual(len(frames), 1)
        self.assertTrue(frames[0].skip_flags & SKIP_NITRO)
        self.assertEqual(frames[0].nitro_activations, 0)

    def test_rpc_is_after_verified_delta_write_and_before_c98(self) -> None:
        region = SOURCE[
            SOURCE.index("bool delta_ready_for_actions = false") :
            SOURCE.index('semantic_fault("unexpected_delta"')
        ]
        self.assertLess(region.index("WriteExactVerified"), region.index("run_prephysics_nitro_rpc"))
        self.assertIn("pending.c98_event != 0", SOURCE)
        self.assertIn("pending.c9c_prefix_event != 0", SOURCE)
        self.assertIn("kFlagObserveOnly", CLIENT)

    def test_payload_keeps_exact_build_object_and_dispatch_gates(self) -> None:
        for evidence in (
            "kExpectedBuildId",
            "kServiceVtableRva",
            "kServiceActivateRva",
            "kMaxRanges = 16384",
            "request.sequence <= last",
            "calls_completed = i + 1",
        ):
            self.assertIn(evidence, PAYLOAD)

    def test_runner_requires_prior_stage2_and_explicit_zero_call_scope(self) -> None:
        for gate in (
            "PayloadStage2ObserveValidated",
            "AcknowledgeNaturallyRunningRaceNoManualInput",
            "AcknowledgeNoPausedAttach",
            "AcknowledgeExactlyOneObserveRequestZeroActivations",
            "AcknowledgeAllControlsActionsAndPhysicsCorrectionSkipped",
            "AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(gate, RUNNER)
        self.assertIn("activations=0", RUNNER)
        self.assertIn("phase=ready", RUNNER)

    def test_runner_does_not_deploy_restart_or_send_input(self) -> None:
        lowered = RUNNER.lower()
        for forbidden in (
            "force-stop",
            "am start",
            "input keyevent",
            "input tap",
            "liba9tas_payload_nitro_rpc_v1.so /data/local/tmp",
        ):
            self.assertNotIn(forbidden, lowered)

    def test_strict_parser_accepts_canonical_observe_report(self) -> None:
        summary = decode_report(self.synthetic_report())
        self.assertEqual(summary.frames, 1)
        self.assertEqual(summary.delta_writes, 1)

    def test_header_abi_keeps_thread_counts_before_rpc_counters(self) -> None:
        original = (
            ROOT
            / "evidence"
            / "a9tas_gate7_steering_report_20260817_182513_762.a9uer5"
        ).read_bytes()
        old_header = list(_HEADER_V5.unpack_from(original))
        header = list(_HEADER.unpack_from(self.synthetic_report()))
        self.assertEqual(header[38:40], old_header[38:40])
        self.assertEqual(header[40:43], [1, 0, 0])

    def test_parser_rejects_activation_and_state_change(self) -> None:
        report = self.synthetic_report()
        header = report[: _HEADER.size]
        frame = list(_FRAME.unpack_from(report, _HEADER.size))
        frame[3] |= 1 << 11
        with self.assertRaisesRegex(ValueError, "frame flags"):
            decode_report(header + _FRAME.pack(*frame))
        frame = list(_FRAME.unpack_from(report, _HEADER.size))
        response = list(_RESPONSE.unpack(frame[27]))
        response[-1] = 1
        frame[27] = _RESPONSE.pack(*response)
        with self.assertRaisesRegex(ValueError, "changed nitro state"):
            decode_report(header + _FRAME.pack(*frame))


if __name__ == "__main__":
    unittest.main()
