#!/usr/bin/env python3
"""Regression tests for the Gate 6 phase-only packet generator/verifier."""

from __future__ import annotations

import json
import struct
import tempfile
import unittest
from pathlib import Path

from make_unified_phase_only_gate_v1 import (
    PHASE_ONLY_SKIP_FLAGS,
    build_phase_only_gate,
    write_new_gate,
)
from native_physics_recording_v1 import NativePhysicsFrameV1, encode_recording as encode_a9nps1
from unified_tick_recording_v1 import UnifiedTickFrameV1, decode_recording, encode_recording
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


def floats(seed: float, count: int) -> bytes:
    return struct.pack(f"<{count}f", *(seed + index for index in range(count)))


def source_blob() -> bytes:
    return encode_a9nps1(
        [
            NativePhysicsFrameV1(40, 100, floats(1.0, 16), floats(20.0, 3)),
            NativePhysicsFrameV1(41, 101, floats(30.0, 16), floats(50.0, 3)),
        ]
    )


class UnifiedPhaseOnlyGateTests(unittest.TestCase):
    def test_builds_exactly_one_all_skip_frame(self) -> None:
        blob, manifest = build_phase_only_gate(source_blob(), frame_index=0, fixed_interval_us=16667)
        interval, frames = decode_recording(blob)
        self.assertEqual((interval, len(frames), frames[0].skip_flags), (16667, 1, 0xFF))
        self.assertEqual(verify_phase_only_gate(blob, manifest)[0], 40)

    def test_preserves_selected_raw_physics_payload(self) -> None:
        source = source_blob()
        blob, _ = build_phase_only_gate(source, frame_index=1, fixed_interval_us=20000)
        _, frames = decode_recording(blob)
        self.assertEqual(frames[0].transform, floats(30.0, 16))
        self.assertEqual(frames[0].linear_velocity, floats(50.0, 3))

    def test_manifest_binds_source_and_output_hashes(self) -> None:
        blob, manifest = build_phase_only_gate(source_blob(), frame_index=0, fixed_interval_us=16667)
        self.assertEqual(manifest["skip_flags"], PHASE_ONLY_SKIP_FLAGS)
        self.assertEqual(verify_phase_only_gate(blob, manifest)[2], manifest["output_sha256"])

    def test_rejects_out_of_range_source_frame(self) -> None:
        with self.assertRaisesRegex(ValueError, "frame_index"):
            build_phase_only_gate(source_blob(), frame_index=2, fixed_interval_us=16667)

    def test_refuses_to_overwrite_either_artifact(self) -> None:
        blob, manifest = build_phase_only_gate(source_blob(), frame_index=0, fixed_interval_us=16667)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "gate.a9utk1"
            manifest_path = root / "gate.json"
            write_new_gate(output, manifest_path, blob, manifest)
            with self.assertRaises(FileExistsError):
                write_new_gate(output, manifest_path, blob, manifest)
            self.assertEqual(json.loads(manifest_path.read_text(encoding="utf-8"))["output_sha256"], manifest["output_sha256"])

    def test_rejects_active_transform(self) -> None:
        blob, _ = build_phase_only_gate(source_blob(), frame_index=0, fixed_interval_us=16667)
        interval, frames = decode_recording(blob)
        frame = frames[0]
        active = UnifiedTickFrameV1(
            frame.tick,
            frame.monotonic_ns,
            frame.steering,
            frame.brake,
            frame.accelerator,
            frame.nitro_activations,
            frame.skip_flags & ~0x80,
            frame.respawn,
            frame.barrel_angular,
            frame.barrel_rbx,
            frame.transform,
            frame.linear_velocity,
        )
        with self.assertRaisesRegex(ValueError, "all eight"):
            verify_phase_only_gate(encode_recording([active], fixed_interval_us=interval))

    def test_rejects_noncanonical_dormant_negative_zero(self) -> None:
        blob, _ = build_phase_only_gate(source_blob(), frame_index=0, fixed_interval_us=16667)
        malformed = bytearray(blob)
        struct.pack_into("<I", malformed, 96 + 16, 0x80000000)
        with self.assertRaisesRegex(ValueError, "canonical"):
            verify_phase_only_gate(bytes(malformed))

    def test_rejects_manifest_claiming_equal(self) -> None:
        blob, manifest = build_phase_only_gate(source_blob(), frame_index=0, fixed_interval_us=16667)
        manifest["does_not_claim"] = []
        with self.assertRaisesRegex(ValueError, "does_not_claim"):
            verify_phase_only_gate(blob, manifest)


if __name__ == "__main__":
    unittest.main()
