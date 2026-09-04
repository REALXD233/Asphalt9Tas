#!/usr/bin/env python3
"""Tests for Gate 8 final-correction-only input and A9UER5 binding."""

from __future__ import annotations

import struct
import unittest

from make_unified_final_correction_gate_v1 import (
    FINAL_CORRECTION_ONLY_SKIP_FLAGS,
    build_final_correction_gate,
)
from make_unified_phase_only_gate_v1 import build_phase_only_gate
from native_physics_recording_v1 import (
    NativePhysicsFrameV1,
    encode_recording as encode_a9nps1,
)
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    BUILD_ID,
    COMMITTED,
    CORRECTION_CORRECTED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    _HEADER,
)
from parse_unified_executor_report_v5 import FRAME_SIZE, HEADER_SIZE, MAGIC, VERSION, _FRAME
from unified_tick_recording_v1 import decode_recording
from verify_gate8_final_correction_result_v1 import verify_gate8_result
from verify_unified_final_correction_gate_v1 import verify_final_correction_gate


def floats(seed: float, count: int) -> bytes:
    return struct.pack(f"<{count}f", *(seed + index for index in range(count)))


def native_blob(seed: float, *, tick: int = 7, time: int = 99) -> bytes:
    return encode_a9nps1(
        [NativePhysicsFrameV1(tick, time, floats(seed, 16), floats(seed + 30, 3))]
    )


def phase_blob() -> bytes:
    return build_phase_only_gate(
        native_blob(1.0), frame_index=0, fixed_interval_us=16667
    )[0]


def input_and_manifest() -> tuple[bytes, dict[str, object], bytes, bytes]:
    phase = phase_blob()
    physics = native_blob(50.0, tick=40, time=500)
    blob, manifest = build_final_correction_gate(
        phase, physics, physics_frame_index=0
    )
    return blob, manifest, phase, physics


def report_for(input_blob: bytes, *, recorded_override: bytes | None = None) -> bytes:
    _, frames = decode_recording(input_blob)
    frame = frames[0]
    recorded = recorded_override or (frame.transform + frame.linear_velocity)
    before_payload = floats(1.0, 16) + floats(31.0, 3)
    before = bytearray(804)
    before[0x10:0x50] = before_payload[:64]
    before[0x150:0x15C] = before_payload[64:]
    immediate = bytearray(before)
    immediate[0x10:0x50] = recorded[:64]
    immediate[0x150:0x15C] = recorded[64:]
    audit = _FRAME.pack(
        frame.tick,
        frame.monotonic_ns,
        300,
        CORRECTION_CORRECTED
        | AUDIT_EXACT
        | GATE2_COMPLETE
        | COMMITTED
        | PREFIX_CERTIFIED,
        16667,
        16667,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        100,
        101,
        1,
        3561,
        bytes(2),
        0,
        bytes(4),
        0,
        0,
        recorded[:64],
        recorded[64:],
        bytes(before),
        bytes(immediate),
    )
    native = 0x77000000
    addresses = (
        2668,
        0x70000000,
        0x71000000,
        0x72000000,
        0x73000000,
        0x730001D0,
        0x730001A0,
        0x74000F64,
        0x75000000,
        0x75000188,
        0x74000000,
        0x76000000,
        native,
        native + 0x10,
        native + 0x150,
    )
    counters = (7, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4)
    header = _HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FRAME_SIZE,
        0x1F,
        1,
        1,
        BUILD_ID,
        0,
        *addresses,
        *counters,
        10,
        14,
    )
    return header + audit


class FinalCorrectionGateTests(unittest.TestCase):
    def test_builds_and_verifies_exact_final_only_input(self) -> None:
        blob, manifest, phase, physics = input_and_manifest()
        tick, interval, _, mismatches = verify_final_correction_gate(
            blob, manifest, phase, physics
        )
        self.assertEqual((tick, interval, mismatches), (7, 16667, 19))
        self.assertEqual(manifest["skip_flags"], FINAL_CORRECTION_ONLY_SKIP_FLAGS)

    def test_rejects_source_equal_to_phase_reference(self) -> None:
        phase = phase_blob()
        with self.assertRaisesRegex(ValueError, "equals the phase reference"):
            build_final_correction_gate(
                phase, native_blob(1.0), physics_frame_index=0
            )

    def test_rejects_manifest_payload_hash_mismatch(self) -> None:
        blob, manifest, phase, physics = input_and_manifest()
        manifest["payload_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "payload_sha256"):
            verify_final_correction_gate(blob, manifest, phase, physics)

    def test_accepts_exact_corrected_a9uer5_cross_binding(self) -> None:
        blob, manifest, phase, physics = input_and_manifest()
        tick, _, mismatches = verify_gate8_result(
            report_for(blob), blob, manifest, phase, physics
        )
        self.assertEqual((tick, mismatches), (7, 19))

    def test_rejects_report_payload_not_bound_to_input(self) -> None:
        blob, manifest, phase, physics = input_and_manifest()
        wrong = floats(80.0, 16) + floats(110.0, 3)
        with self.assertRaisesRegex(ValueError, "not bound"):
            verify_gate8_result(
                report_for(blob, recorded_override=wrong),
                blob,
                manifest,
                phase,
                physics,
            )


if __name__ == "__main__":
    unittest.main()
