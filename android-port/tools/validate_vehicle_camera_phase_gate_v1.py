#!/usr/bin/env python3
"""Validate one finalized A9VCPTR1 360-frame phase Gate report."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

import analyze_vehicle_camera_phase_events_v1 as analyzer


REPORT_SIZE = 376
EVIDENCE_OFFSET = 184
REQUIRED_FLAGS = 0x3DF
EVIDENCE_PREFIX = struct.Struct("<8sII" + "Q" * 11 + "IIII" + "QQQ")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate(data: bytes, expected_frames: int = 360,
             expected_maximum_events: int = 4096) -> dict[str, int]:
    require(len(data) >= REPORT_SIZE, "short A9VCPTR1 report")
    require(data[:8] == b"A9VCPTR1", "wrong transaction magic")
    version, size, action, flags = struct.unpack_from("<IIII", data, 8)
    require(version == 1 and size == REPORT_SIZE and action == 3,
            "report is not a finalized v1 transaction")
    require(flags & REQUIRED_FLAGS == REQUIRED_FLAGS,
            f"required transaction flags missing: {flags:#x}")
    maximum_events, copied_events = struct.unpack_from("<II", data, 136)
    read_errors, semantic_errors = struct.unpack_from("<QQ", data, 168)
    require(maximum_events == expected_maximum_events,
            "unexpected event bound")
    require(read_errors == 0 and semantic_errors == 0,
            "transaction reported read/semantic errors")
    require(len(data) == REPORT_SIZE + copied_events * analyzer.EVENT.size,
            "report/event file length mismatch")

    values = EVIDENCE_PREFIX.unpack_from(data, EVIDENCE_OFFSET)
    (magic, evidence_version, evidence_size, next_sequence, committed,
     dropped, camera_entries, camera_calls, camera_returns, camera_failures,
     camera_recursive, vehicle_before, vehicle_after, vehicle_corrected,
     camera_frames, last_status, last_tid, reserved0, last_manager, last_node,
     last_shape) = values
    require(magic == b"A9VCPE1\0" and evidence_version == 1 and
            evidence_size == 192, "invalid phase evidence ABI")
    require(next_sequence == committed == copied_events,
            "phase stream is not fully committed and contiguous")
    require(dropped == 0 and camera_failures == 0 and camera_recursive == 0,
            "phase payload reported failure, recursion, or overflow")
    require(camera_entries == camera_calls == camera_returns == camera_frames,
            "camera wrapper did not preserve one original call per entry")
    require(camera_entries > 0, "no RaceView callback was observed")
    require(vehicle_before == vehicle_after == expected_frames,
            "vehicle wrapper did not produce one complete bracket per frame")
    require(vehicle_corrected <= expected_frames and last_status == 1 and
            last_tid != 0 and reserved0 == 0,
            "phase evidence terminal state invalid")
    require(last_manager != 0 and last_node != 0 and last_shape != 0,
            "camera identity receipt missing")

    events = analyzer.read_transaction_events_from_bytes(data)
    analysis = analyzer.analyze(events)
    summary = analysis["summary"]
    require(summary["complete_vehicle_brackets"] == expected_frames,
            "analyzer did not recover every vehicle callback bracket")
    require(summary["vehicle_before_events"] == expected_frames and
            summary["vehicle_after_events"] == expected_frames,
            "analyzer vehicle event totals mismatch")
    require(summary["camera_events"] == camera_entries,
            "analyzer/evidence camera totals mismatch")
    require(summary["corrected_vehicle_frames"] == vehicle_corrected,
            "analyzer/evidence corrected totals mismatch")
    require(summary["sequence_gaps"] == 0 and
            summary["duplicate_sequences"] == 0 and
            summary["unknown_kind_events"] == 0,
            "event stream sequence or kind invalid")
    return {
        "events": copied_events,
        "camera": camera_entries,
        "corrected": vehicle_corrected,
        "camera_inside": summary["camera_phase_counts"].get(
            "inside_vehicle_callback", 0),
        "camera_after": summary["camera_phase_counts"].get(
            "after_vehicle_before_next", 0),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("--frames", type=int, default=360)
    parser.add_argument("--maximum-events", type=int, default=4096)
    args = parser.parse_args()
    try:
        result = validate(args.report.read_bytes(), args.frames,
                          args.maximum_events)
    except (OSError, ValueError, struct.error) as error:
        print(f"vehicle_camera_phase_gate_error={error}", file=sys.stderr)
        return 1
    print("VEHICLE_CAMERA_PHASE_GATE_VALID passed=1 " + " ".join(
        f"{key}={value}" for key, value in result.items()) +
          " camera_write=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
