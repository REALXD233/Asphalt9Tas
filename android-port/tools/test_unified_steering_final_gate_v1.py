#!/usr/bin/env python3
"""Tests for Gate 9 three-frame steering+final input and A9UER5 binding."""

from __future__ import annotations

import struct
import unittest

from make_unified_phase_only_gate_v1 import build_phase_only_gate
from make_unified_steering_final_gate_v1 import (
    STEERING_FINAL_SKIP_FLAGS,
    build_steering_final_gate,
)
from make_unified_steering_gate_v1 import _A9SPR1_FRAME, _A9SPR1_HEADER, _BUILD_ID
from native_physics_recording_v1 import NativePhysicsFrameV1, encode_recording as encode_a9nps1
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    BUILD_ID,
    COMMITTED,
    CORRECTION_CORRECTED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    STEERING_APPLIED,
    _HEADER,
)
from parse_unified_executor_report_v5 import FRAME_SIZE, HEADER_SIZE, MAGIC, VERSION, _FRAME
from unified_tick_recording_v1 import decode_recording
from verify_gate9_steering_final_result_v1 import verify_gate9_result
from verify_unified_steering_final_gate_v1 import verify_steering_final_gate


def floats(seed: float, count: int) -> bytes:
    return struct.pack(f"<{count}f", *(seed + index for index in range(count)))


def phase_blob() -> bytes:
    source = encode_a9nps1(
        [NativePhysicsFrameV1(7, 99, floats(1.0, 16), floats(31.0, 3))]
    )
    return build_phase_only_gate(source, frame_index=0, fixed_interval_us=16667)[0]


def direction_blob() -> bytes:
    count = 30
    output = bytearray(
        _A9SPR1_HEADER.pack(b"A9SPR1\0\0", 1, 64, 16, count, _BUILD_ID, bytes(20))
    )
    selected = {25: 0.5, 26: 0.55, 27: 0.6}
    for index in range(count):
        output += _A9SPR1_FRAME.pack(selected.get(index, 0.0), 0.0, 0.0, 7)
    return bytes(output)


def physics_blob() -> bytes:
    return encode_a9nps1(
        [
            NativePhysicsFrameV1(40 + index, 500 + index, floats(seed, 16), floats(seed + 30, 3))
            for index, seed in enumerate((50.0, 80.0, 100.0, 110.0))
        ]
    )


def input_and_manifest() -> tuple[bytes, dict[str, object], bytes, bytes, bytes]:
    phase = phase_blob()
    direction = direction_blob()
    physics = physics_blob()
    blob, manifest = build_steering_final_gate(
        phase,
        direction,
        physics,
        direction_indices=(25, 26, 27),
        physics_indices=(0, 1, 3),
    )
    return blob, manifest, phase, direction, physics


def report_for(input_blob: bytes, *, override_index: int | None = None) -> bytes:
    _, input_frames = decode_recording(input_blob)
    reports: list[bytes] = []
    event = 0
    for index, input_frame in enumerate(input_frames):
        recorded = input_frame.transform + input_frame.linear_velocity
        if override_index == index:
            recorded = floats(200.0, 16) + floats(230.0, 3)
        before_payload = floats(-100.0 - 30 * index, 16) + floats(-70.0 - 30 * index, 3)
        before = bytearray(804)
        before[0x10:0x50] = before_payload[:64]
        before[0x150:0x15C] = before_payload[64:]
        immediate = bytearray(before)
        immediate[0x10:0x50] = recorded[:64]
        immediate[0x150:0x15C] = recorded[64:]
        bits = int.from_bytes(struct.pack("<f", input_frame.steering), "little")
        events = tuple(event + offset for offset in range(1, 8))
        event = events[-1]
        reports.append(
            _FRAME.pack(
                input_frame.tick,
                input_frame.monotonic_ns,
                300 + index,
                STEERING_APPLIED
                | CORRECTION_CORRECTED
                | AUDIT_EXACT
                | GATE2_COMPLETE
                | COMMITTED
                | PREFIX_CERTIFIED,
                16667,
                16667,
                *events,
                100 + index,
                101 + index,
                1,
                3561 + index,
                bytes(2),
                bits,
                bytes(4),
                (bits << 32) | 0x11111111,
                (bits << 32) | 0x22222222,
                recorded[:64],
                recorded[64:],
                bytes(before),
                bytes(immediate),
            )
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
    counters = (event, 3, 6, 0, 3, 0, 0, 0, 0, 3, 0, 0, 0, 4)
    header = _HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FRAME_SIZE,
        0x1F,
        3,
        3,
        BUILD_ID,
        0,
        *addresses,
        *counters,
        10,
        14,
    )
    return header + b"".join(reports)


class SteeringFinalGateTests(unittest.TestCase):
    def test_builds_and_verifies_three_exact_frames(self) -> None:
        blob, manifest, phase, direction, physics = input_and_manifest()
        first, last, interval, _, bits, mismatches = verify_steering_final_gate(
            blob, manifest, phase, direction, physics
        )
        self.assertEqual((first, last, interval), (7, 9, 16667))
        self.assertEqual(bits, ("0000003f", "cdcc0c3f", "9a99193f"))
        self.assertEqual(mismatches, (19, 19, 19))
        self.assertEqual(manifest["skip_flags"], STEERING_FINAL_SKIP_FLAGS)

    def test_rejects_duplicate_physics_targets(self) -> None:
        with self.assertRaisesRegex(ValueError, "unique"):
            build_steering_final_gate(
                phase_blob(),
                direction_blob(),
                physics_blob(),
                direction_indices=(25, 26, 27),
                physics_indices=(0, 1, 1),
            )

    def test_rejects_manifest_steering_bits_tamper(self) -> None:
        blob, manifest, phase, direction, physics = input_and_manifest()
        manifest["steering_bits_hex"][1] = "00000000"
        with self.assertRaisesRegex(ValueError, "steering_bits_hex"):
            verify_steering_final_gate(blob, manifest, phase, direction, physics)

    def test_accepts_exact_three_frame_a9uer5_binding(self) -> None:
        blob, manifest, phase, direction, physics = input_and_manifest()
        first, last, _, _ = verify_gate9_result(
            report_for(blob), blob, manifest, phase, direction, physics
        )
        self.assertEqual((first, last), (7, 9))

    def test_rejects_one_report_payload_not_bound_to_input(self) -> None:
        blob, manifest, phase, direction, physics = input_and_manifest()
        with self.assertRaisesRegex(ValueError, "frame 1: correction payload is not bound"):
            verify_gate9_result(
                report_for(blob, override_index=1),
                blob,
                manifest,
                phase,
                direction,
                physics,
            )


if __name__ == "__main__":
    unittest.main()
