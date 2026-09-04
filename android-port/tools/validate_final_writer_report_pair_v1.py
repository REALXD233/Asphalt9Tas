#!/usr/bin/env python3
"""Cross-validate A9UER8 and A9FWR1 as one final-writer transaction."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

import parse_unified_executor_report_v8 as unified_v8
import validate_final_writer_payload_report_v1 as payload_v1
from parse_unified_executor_report_v2 import HEADER_SIZE as UNIFIED_HEADER_SIZE, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE as UNIFIED_FRAME_SIZE, _FRAME


CORRECTION_EQUAL = 1 << 1
CORRECTION_CORRECTED = 1 << 2
CORRECTION_SKIPPED = 1 << 3
PAYLOAD_EQUAL = 1 << 1
PAYLOAD_CORRECTED = 1 << 2
TRANSFORM_OFFSET = 0x10
TRANSFORM_SIZE = 64
LINEAR_OFFSET = 0x150
LINEAR_SIZE = 12


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_pair(unified: bytes, payload_report: bytes, target_blob: bytes,
                  payload: bytes | None = None) -> dict[str, int]:
    unified_summary = unified_v8.decode_report(unified)
    payload_summary = payload_v1.validate_report(
        payload_report, target_blob, payload)
    unified_header = _HEADER.unpack_from(unified)
    payload_header = payload_v1._HEADER.unpack_from(payload_report)
    frame_count = unified_header[5]
    _require(frame_count == payload_header[5] == payload_summary["frames"],
             "A9UER8/A9FWR1 frame count mismatch")
    _require(unified_header[9] == payload_header[7] and
             unified_header[10] == payload_header[8] and
             unified_header[19] == payload_header[9],
             "A9UER8/A9FWR1 process or player identity mismatch")
    _require(unified_header[27] == payload_summary["equal"] and
             unified_header[28] == payload_summary["corrected"] and
             unified_header[29] == 0 and
             unified_header[33] == payload_summary["corrected"],
             "A9UER8/A9FWR1 correction totals mismatch")

    unified_cursor = UNIFIED_HEADER_SIZE
    payload_cursor = payload_v1.HEADER_SIZE
    for index in range(frame_count):
        unified_frame = _FRAME.unpack_from(unified, unified_cursor)
        payload_audit = payload_v1._AUDIT.unpack_from(
            payload_report, payload_cursor)
        unified_cursor += UNIFIED_FRAME_SIZE
        payload_cursor += payload_v1.AUDIT_SIZE
        flags = unified_frame[3]
        payload_flags = payload_audit[1]
        unified_equal = bool(flags & CORRECTION_EQUAL)
        unified_corrected = bool(flags & CORRECTION_CORRECTED)
        _require(not (flags & CORRECTION_SKIPPED) and
                 unified_equal != unified_corrected,
                 f"frame {index}: A9UER8 correction class invalid")
        _require(unified_equal == bool(payload_flags & PAYLOAD_EQUAL) and
                 unified_corrected == bool(payload_flags & PAYLOAD_CORRECTED),
                 f"frame {index}: correction class differs between reports")
        recorded_transform, recorded_linear = unified_frame[22:24]
        before_snapshot, immediate_snapshot = unified_frame[24:26]
        (_, _, before_transform, before_linear,
         immediate_transform, immediate_linear) = payload_audit
        _require(recorded_transform == immediate_transform and
                 recorded_linear == immediate_linear,
                 f"frame {index}: unified target differs from payload immediate")
        _require(before_snapshot[TRANSFORM_OFFSET:TRANSFORM_OFFSET + TRANSFORM_SIZE]
                 == before_transform and
                 before_snapshot[LINEAR_OFFSET:LINEAR_OFFSET + LINEAR_SIZE]
                 == before_linear,
                 f"frame {index}: before snapshot provenance mismatch")
        _require(immediate_snapshot[
                     TRANSFORM_OFFSET:TRANSFORM_OFFSET + TRANSFORM_SIZE]
                 == immediate_transform and
                 immediate_snapshot[LINEAR_OFFSET:LINEAR_OFFSET + LINEAR_SIZE]
                 == immediate_linear,
                 f"frame {index}: immediate snapshot provenance mismatch")
    _require(unified_summary.frames == frame_count,
             "A9UER8 delegated summary mismatch")
    return {"frames": frame_count,
            "equal": int(payload_summary["equal"]),
            "corrected": int(payload_summary["corrected"])}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("unified_report", type=pathlib.Path)
    parser.add_argument("payload_report", type=pathlib.Path)
    parser.add_argument("target_blob", type=pathlib.Path)
    parser.add_argument("payload", nargs="?", type=pathlib.Path)
    args = parser.parse_args()
    try:
        summary = validate_pair(
            args.unified_report.read_bytes(), args.payload_report.read_bytes(),
            args.target_blob.read_bytes(),
            args.payload.read_bytes() if args.payload is not None else None)
    except (OSError, ValueError, struct.error) as error:
        print(f"final_writer_report_pair_error={error}", file=sys.stderr)
        return 1
    print("FINAL_WRITER_REPORT_PAIR_VALID passed=1 " + " ".join(
        f"{key}={value}" for key, value in summary.items()) +
          " same_process=1 same_player=1 per_frame_provenance=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
