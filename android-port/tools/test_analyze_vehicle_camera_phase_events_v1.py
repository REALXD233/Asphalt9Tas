#!/usr/bin/env python3

from __future__ import annotations

import dataclasses
import importlib.util
import pathlib
import sys
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name(
    "analyze_vehicle_camera_phase_events_v1.py")
SPEC = importlib.util.spec_from_file_location("phase_analyzer", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def event(sequence: int, kind: int, vehicle_frame: int,
          camera_frame: int = 0xFFFFFFFF, flags: int = 0,
          tid: int = 10, monotonic_ns: int | None = None,
          pose_hash: int = 0, linear_hash: int = 0):
    return MODULE.Event(sequence, sequence if monotonic_ns is None else monotonic_ns,
                        kind, flags, tid, vehicle_frame, camera_frame, 0,
                        pose_hash, linear_hash, 0, 0, 0, 0)


class PhaseAnalyzerTests(unittest.TestCase):
    def test_classifies_inside_and_post_vehicle_camera_callbacks(self) -> None:
        events = [
            event(0, MODULE.VEHICLE_BEFORE, 0),
            event(1, MODULE.CAMERA_AFTER_ORIGINAL, 0, 0, tid=20),
            event(2, MODULE.VEHICLE_AFTER, 0,
                  flags=MODULE.FLAG_VEHICLE_CORRECTED,
                  pose_hash=101, linear_hash=102),
            event(3, MODULE.CAMERA_AFTER_ORIGINAL, 1, 1, tid=20,
                  flags=MODULE.FLAG_VEHICLE_SNAPSHOT_PRESENT,
                  pose_hash=101, linear_hash=102),
            event(4, MODULE.VEHICLE_BEFORE, 1),
            event(5, MODULE.VEHICLE_AFTER, 1,
                  flags=MODULE.FLAG_VEHICLE_EQUAL |
                        MODULE.FLAG_VEHICLE_IMMEDIATE_EXACT),
        ]
        report = MODULE.analyze(reversed(events))
        summary = report["summary"]
        self.assertEqual(summary["complete_vehicle_brackets"], 2)
        self.assertEqual(summary["corrected_vehicle_frames"], 1)
        self.assertEqual(summary["equal_vehicle_frames"], 1)
        self.assertEqual(summary["camera_phase_counts"], {
            "after_vehicle_before_next": 1,
            "inside_vehicle_callback": 1,
        })
        self.assertEqual(report["frames"][0]["camera_inside"], 1)
        self.assertEqual(report["frames"][0]["camera_after_before_next"], 1)
        self.assertEqual(summary["vehicle_tids"], [10])
        self.assertEqual(summary["camera_tids"], [20])
        self.assertEqual(summary["post_vehicle_camera_snapshots"], 1)
        self.assertEqual(summary["post_vehicle_camera_both_exact"], 1)
        self.assertEqual(summary["post_vehicle_camera_changed"], 0)

    def test_reports_malformed_stream_without_inventing_brackets(self) -> None:
        report = MODULE.analyze([
            event(0, MODULE.VEHICLE_AFTER, 9),
            event(2, 99, 0),
            event(2, MODULE.CAMERA_AFTER_ORIGINAL, 9, 0,
                  monotonic_ns=0),
        ])
        summary = report["summary"]
        self.assertEqual(summary["complete_vehicle_brackets"], 0)
        self.assertEqual(summary["sequence_gaps"], 1)
        self.assertEqual(summary["unknown_kind_events"], 1)

    def test_reports_three_stage_camera_bursts(self) -> None:
        snapshot = MODULE.FLAG_VEHICLE_SNAPSHOT_PRESENT
        events = [
            event(0, MODULE.VEHICLE_BEFORE, 0),
            event(1, MODULE.VEHICLE_AFTER, 0, pose_hash=10, linear_hash=20),
            event(2, MODULE.CAMERA_AFTER_ORIGINAL, 1, 0, flags=snapshot,
                  pose_hash=10, linear_hash=20),
            event(3, MODULE.CAMERA_AFTER_ORIGINAL, 1, 1, flags=snapshot,
                  pose_hash=10, linear_hash=20),
            event(4, MODULE.CAMERA_AFTER_ORIGINAL, 1, 2, flags=snapshot,
                  pose_hash=10, linear_hash=20),
            event(5, MODULE.VEHICLE_BEFORE, 1),
            event(6, MODULE.VEHICLE_AFTER, 1, pose_hash=11, linear_hash=21),
            event(7, MODULE.CAMERA_AFTER_ORIGINAL, 2, 3, flags=snapshot,
                  pose_hash=11, linear_hash=21),
            event(8, MODULE.CAMERA_AFTER_ORIGINAL, 2, 4, flags=snapshot,
                  pose_hash=11, linear_hash=21),
            event(9, MODULE.CAMERA_AFTER_ORIGINAL, 2, 5, flags=snapshot,
                  pose_hash=11, linear_hash=21),
        ]
        world = [100, 101, 101, 102, 103, 103]
        shape = [90, 90, 90, 101, 101, 101]
        fov = [5, 5, 6, 6, 6, 7]
        events = [dataclasses.replace(item,
                                      camera_world_hash=world[item.camera_frame],
                                      camera_shape_hash=shape[item.camera_frame],
                                      camera_fov_bits=fov[item.camera_frame])
                  if item.kind == MODULE.CAMERA_AFTER_ORIGINAL else item
                  for item in events]
        burst = MODULE.analyze(events)["summary"]["camera_burst_analysis"]
        self.assertEqual(burst["complete_three_callback_groups"], 2)
        self.assertEqual(burst["world_pattern_counts"], {"011": 2})
        self.assertEqual(burst["shape_pattern_counts"], {"000": 2})
        self.assertEqual(burst["fov_pattern_counts"], {"001": 2})
        self.assertEqual(burst["pre_fov_equals_previous_final"], 1)
        self.assertEqual(burst["shape_equals_previous_final_world"], 1)
        self.assertEqual(burst["vehicle_snapshot_by_slot"], [
            {"slot": 0, "snapshots": 2, "pose_exact": 2,
             "linear_exact": 2, "both_exact": 2, "changed": 0},
            {"slot": 1, "snapshots": 2, "pose_exact": 2,
             "linear_exact": 2, "both_exact": 2, "changed": 0},
            {"slot": 2, "snapshots": 2, "pose_exact": 2,
             "linear_exact": 2, "both_exact": 2, "changed": 0},
        ])
        self.assertEqual(
            burst["vehicle_snapshot_first_change_slot_counts"],
            {"none": 2})

    def test_raw_reader_uses_exact_80_byte_abi_and_committed_bound(self) -> None:
        packed = MODULE.EVENT.pack(0, 100, MODULE.VEHICLE_BEFORE, 0, 7, 0,
                                   0xFFFFFFFF, 0, 1, 2, 0, 0, 0, 0)
        packed += bytes(MODULE.EVENT.size)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.bin"
            path.write_bytes(packed)
            parsed = MODULE.read_events(path, committed=1)
            self.assertEqual(MODULE.EVENT.size, 80)
            self.assertEqual(len(parsed), 1)
            self.assertEqual(parsed[0].producer_tid, 7)

    def test_transaction_reader_validates_envelope_and_extracts_events(self) -> None:
        packed_event = MODULE.EVENT.pack(
            0, 100, MODULE.CAMERA_AFTER_ORIGINAL, 0, 7, 0, 0, 0,
            0, 0, 1, 2, 3, 0)
        envelope = bytearray(MODULE.TRANSACTION_REPORT_SIZE)
        envelope[:8] = b"A9VCPTR1"
        import struct
        struct.pack_into("<II", envelope, 8, 1,
                         MODULE.TRANSACTION_REPORT_SIZE)
        struct.pack_into("<I", envelope,
                         MODULE.TRANSACTION_MAXIMUM_EVENTS_OFFSET, 16)
        struct.pack_into("<I", envelope,
                         MODULE.TRANSACTION_COPIED_EVENTS_OFFSET, 1)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "transaction.bin"
            path.write_bytes(bytes(envelope) + packed_event)
            parsed = MODULE.read_transaction_events(path)
            self.assertEqual(len(parsed), 1)
            self.assertEqual(parsed[0].camera_shape_hash, 2)


if __name__ == "__main__":
    unittest.main()
