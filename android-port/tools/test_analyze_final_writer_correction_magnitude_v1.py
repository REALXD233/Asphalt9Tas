#!/usr/bin/env python3
"""Unit tests for the A9FWR1 correction-magnitude analyzer."""

from __future__ import annotations

import hashlib
import struct

import analyze_final_writer_correction_magnitude_v1 as analyzer
import make_final_writer_target_blob_v1 as target_blob_v1
import validate_final_writer_payload_report_v1 as final_writer_v1


def _transform(x: float) -> bytes:
    values = [1.0, 0.0, 0.0, 0.0,
              0.0, 1.0, 0.0, 0.0,
              0.0, 0.0, 1.0, 0.0,
              x, 0.0, 0.0, 1.0]
    return struct.pack("<16f", *values)


def _build() -> tuple[bytes, bytes]:
    source_hash = hashlib.sha256(b"source").digest()
    targets = [(_transform(0.0), struct.pack("<3f", 1.0, 0.0, 0.0)),
               (_transform(1.0), struct.pack("<3f", 1.0, 0.0, 0.0)),
               (_transform(2.0), struct.pack("<3f", 1.0, 0.0, 0.0))]
    target = bytearray(target_blob_v1._HEADER.pack(
        target_blob_v1.MAGIC, 1, target_blob_v1.HEADER_SIZE,
        target_blob_v1.RECORD_SIZE, 3, target_blob_v1.REQUIRED_INTERVAL_US,
        target_blob_v1.REQUIRED_FLAGS, 1, 0, source_hash,
        target_blob_v1.SUPPORTED_BUILD_ID, bytes(36)))
    for transform, linear in targets:
        target += target_blob_v1._TARGET.pack(transform, linear, 0)

    shadow_vptr = 0x2200
    original_vptr = 0x1100
    evidence_prefix = final_writer_v1._EVIDENCE_PREFIX.pack(
        final_writer_v1.EVIDENCE_MAGIC, 1, final_writer_v1.EVIDENCE_SIZE,
        3, 3, 3, 1, 2, 4, 0, 0, 3, 1, 0x3300, 0x4400,
        shadow_vptr, original_vptr, 7, 0, 0, 0)
    evidence = evidence_prefix + bytes(
        final_writer_v1.EVIDENCE_SIZE - len(evidence_prefix))
    report = bytearray(final_writer_v1._HEADER.pack(
        final_writer_v1.MAGIC, 1, final_writer_v1.HEADER_SIZE,
        final_writer_v1.AUDIT_SIZE, final_writer_v1.REQUIRED_FLAGS, 3, 0,
        123, 0x100000, 0x3300, 0x5500, shadow_vptr, original_vptr,
        0x6600, 0x7700, 0x8800, 0x9900, source_hash, bytes(32), evidence))
    before = [targets[0], (targets[0][0], targets[1][1]),
              (_transform(1.5), struct.pack("<3f", 2.0, 0.0, 0.0))]
    for index, ((target_transform, target_linear),
                (before_transform, before_linear)) in enumerate(zip(targets, before)):
        equal = before_transform == target_transform and before_linear == target_linear
        flags = (final_writer_v1.AUDIT_ORIGINAL_RETURNED |
                 final_writer_v1.AUDIT_IMMEDIATE_EXACT |
                 (final_writer_v1.AUDIT_EQUAL if equal else
                  final_writer_v1.AUDIT_CORRECTED))
        if index == 2:
            flags |= (final_writer_v1.AUDIT_FINAL_FRAME |
                      final_writer_v1.AUDIT_VPTR_RESTORED)
        report += final_writer_v1._AUDIT.pack(
            index, flags, before_transform, before_linear,
            target_transform, target_linear)
    return bytes(report), bytes(target)


def main() -> None:
    report, target = _build()
    result = analyzer.analyze(report, target)
    assert result["frames"] == 3
    assert result["equal_frames"] == 1
    assert result["corrected_frames"] == 2
    assert result["transform_mismatch_frames"] == 2
    assert result["linear_mismatch_frames"] == 1
    assert result["longest_consecutive_correction_run"] == 2
    assert result["position_error_corrected"]["max"] == 1.0
    assert result["target_position_step"]["p50"] == 1.0
    assert result["position_error_threshold_counts"]["gt_0_5"] == 1
    assert result["top_position_error_frames"][0] == {"frame": 1, "error": 1.0}
    assert result["linear_velocity_error_corrected"]["max"] == 1.0
    assert sum(result["nearest_position_target"].values()) + result[
        "nearest_position_target_ties"] == 3
    print("CORRECTION_MAGNITUDE_ANALYZER_TEST passed=1")


if __name__ == "__main__":
    main()
