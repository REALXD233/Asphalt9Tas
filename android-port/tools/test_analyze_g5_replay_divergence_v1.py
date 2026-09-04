#!/usr/bin/env python3
"""Unit tests for the source-bound A9G5D1 divergence analyzer."""

from __future__ import annotations

import hashlib
import struct

import analyze_g5_replay_divergence_v1 as analyzer


def _transform(x: float) -> bytes:
    return struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0,
                       0, 0, 1, 0, x, 0, 0, 1)


def _source() -> tuple[bytes, list[tuple[bytes, bytes]]]:
    targets = [
        (_transform(0.0), struct.pack("<3f", 1, 0, 0)),
        (_transform(1.0), struct.pack("<3f", 1, 0, 0)),
        (_transform(2.0), struct.pack("<3f", 1, 0, 0)),
    ]
    data = bytearray(analyzer.SOURCE_HEADER.pack(
        analyzer.SOURCE_MAGIC, 2, analyzer.SOURCE_HEADER.size,
        analyzer.SOURCE_FRAME.size, analyzer.SOURCE_INTERVAL.size,
        3, 3, 16667, 0x0F, 7, 1, 0, bytes(8)))
    for index, (transform, linear) in enumerate(targets):
        data += analyzer.SOURCE_FRAME.pack(
            index, index * 16667 * 1000, 0.0, 0.0, 0.0, 0, 0x78,
            0, bytes(3), 0.0, 0.0, 0.0, 0.0, 0.0,
            transform, linear, 0x7, 0)
    interval_bits = struct.unpack("<I", struct.pack("<f", 1.0 / 60.0))[0]
    half_interval_bits = struct.unpack(
        "<I", struct.pack("<f", 1.0 / 120.0))[0]
    for index in range(3):
        bits = half_interval_bits if index == 2 else interval_bits
        data += analyzer.SOURCE_INTERVAL.pack(index, 0, bits)
    return bytes(data), targets


def _diagnostic(source: bytes,
                targets: list[tuple[bytes, bytes]]) -> bytes:
    signed_zero_transform = bytearray(targets[0][0])
    signed_zero_transform[4:8] = struct.pack("<f", -0.0)
    natural = [
        (bytes(signed_zero_transform), targets[0][1]),
        (_transform(0.5), targets[1][1]),
        (targets[2][0], struct.pack("<3f", 2, 0, 0)),
    ]
    data = bytearray(analyzer.DIAGNOSTIC_HEADER.pack(
        analyzer.DIAGNOSTIC_MAGIC, 1, analyzer.DIAGNOSTIC_HEADER.size,
        analyzer.DIAGNOSTIC_RECORD.size, 3, 16667,
        analyzer.DIAGNOSTIC_FLAGS, 9, 1, 0,
        hashlib.sha256(source).digest(), bytes(16)))
    for index, (transform, linear) in enumerate(natural):
        flags = analyzer.FLAG_EQUAL if (
            analyzer._component_equal_raw(transform, targets[index][0]) and
            analyzer._component_equal_raw(linear, targets[index][1])) else \
            analyzer.FLAG_CORRECTED
        data += analyzer.DIAGNOSTIC_RECORD.pack(
            index, flags, 0, transform, linear, bytes(4))
    return bytes(data)


def main() -> None:
    source, targets = _source()
    diagnostic = _diagnostic(source, targets)
    receipts = (
        ",".join(analyzer.TICK_RECEIPT_COLUMNS) + "\n" +
        "0,1,1,1,0,0,0,0\n" +
        "1,2,1,1,0,0,0,0\n" +
        "2,1,1,1,0,0,0,0\n"
    )
    result = analyzer.analyze(diagnostic, source, receipts)
    assert result["frames"] == 3
    assert result["equal_frames"] == 1
    assert result["corrected_frames"] == 2
    assert result["first_divergence_frame"] == 1
    assert result["first_transform_divergence_frame"] == 1
    assert result["first_linear_divergence_frame"] == 2
    assert result["transform_mismatch_frames"] == 1
    assert result["linear_mismatch_frames"] == 1
    assert result["raw_transform_mismatch_frames"] == 2
    assert result["raw_linear_mismatch_frames"] == 1
    assert result["raw_byte_only_frames"] == 1
    assert result["exact_full_state_neighbor"] == {
        "previous": 0, "current": 1, "next": 0, "none": 2}
    assert result["nearest_full_state_neighbor"] == {
        "previous": 0, "current": 2, "next": 0}
    assert result["nearest_full_state_neighbor_ties"] == 1
    assert result["tick_receipts_present"]
    assert result["tick_receipt_totals"]["physics_interval_calls"] == 4
    assert result["interval_call_delta_groups"]["+0"]["frames"] == 2
    assert result["interval_call_delta_groups"]["+1"]["frames"] == 1
    assert result["longest_consecutive_correction_run"] == 2
    assert result["longest_correction_runs"][0] == {
        "start": 1, "end": 2, "length": 2}
    assert result["early_divergent_frames"][0]["frame"] == 1
    assert result["first_position_error_over"]["1e-1"] == 1
    assert result["source_interval_calls_by_bits"] == {
        "0x3c088889": 1, "0x3c888889": 2}
    assert result["source_interval_frames_by_bits"] == {
        "0x3c088889": 1, "0x3c888889": 2}
    assert result["source_interval_error_groups"]["0x3c088889"][
        "corrected_frames"] == 1
    corrupted = bytearray(source)
    corrupted[-1] ^= 1
    try:
        analyzer.analyze(diagnostic, bytes(corrupted))
    except ValueError as error:
        assert "SHA-256" in str(error)
    else:
        raise AssertionError("source hash mismatch accepted")
    print("G5_REPLAY_DIVERGENCE_ANALYZER_TEST passed=1")


if __name__ == "__main__":
    main()
