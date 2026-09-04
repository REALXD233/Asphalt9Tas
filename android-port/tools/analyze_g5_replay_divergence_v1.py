#!/usr/bin/env python3
"""Validate A9G5D1 and locate natural-before-correction replay divergence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import pathlib
import statistics
import struct
import sys


DIAGNOSTIC_HEADER = struct.Struct("<8sIIIIIIQII32s16s")
DIAGNOSTIC_RECORD = struct.Struct("<QII64s12s4s")
SOURCE_HEADER = struct.Struct("<8sIIIIIIIIQII8s")
SOURCE_FRAME = struct.Struct("<QQfffIIB3s3f2f64s12sII")
SOURCE_INTERVAL = struct.Struct("<QII")

DIAGNOSTIC_MAGIC = b"A9G5D1\0\0"
SOURCE_MAGIC = b"A9G4R2\0\0"
DIAGNOSTIC_FLAGS = 0x3
FLAG_EQUAL = 0x1
FLAG_CORRECTED = 0x2
TICK_RECEIPT_COLUMNS = (
    "tick", "physics_interval_calls", "steering_calls", "brake_calls",
    "accelerator_calls", "natural_nitro_calls", "suppressed_nitro_calls",
    "injected_nitro_calls",
)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _floats(raw: bytes) -> tuple[float, ...]:
    return struct.unpack(f"<{len(raw) // 4}f", raw)


def _finite(raw: bytes) -> bool:
    return all(math.isfinite(value) and abs(value) <= 1_000_000.0
               for value in _floats(raw))


def _component_equal_raw(lhs: bytes, rhs: bytes) -> bool:
    """Match AluTasV2's component-wise C++ float operator== semantics."""
    return len(lhs) == len(rhs) and len(lhs) % 4 == 0 and all(
        left == right for left, right in zip(_floats(lhs), _floats(rhs)))


def _norm(values: list[float] | tuple[float, ...]) -> float:
    return math.sqrt(sum(value * value for value in values))


def _percentiles(values: list[float]) -> dict[str, float]:
    if not values:
        return {"p50": 0.0, "p90": 0.0, "p99": 0.0, "max": 0.0}
    ordered = sorted(values)

    def select(fraction: float) -> float:
        return ordered[round((len(ordered) - 1) * fraction)]

    return {
        "p50": statistics.median(ordered),
        "p90": select(0.90),
        "p99": select(0.99),
        "max": ordered[-1],
    }


def _longest_true_run(values: list[bool]) -> int:
    longest = 0
    current = 0
    for value in values:
        current = current + 1 if value else 0
        longest = max(longest, current)
    return longest


def _true_runs(values: list[bool]) -> list[dict[str, int]]:
    runs: list[dict[str, int]] = []
    start = -1
    for index, value in enumerate(values + [False]):
        if value and start < 0:
            start = index
        elif not value and start >= 0:
            runs.append({"start": start, "end": index - 1,
                         "length": index - start})
            start = -1
    return sorted(runs, key=lambda run: (-run["length"], run["start"]))


def _first_over(values: list[float], threshold: float) -> int:
    return next((index for index, value in enumerate(values)
                 if value > threshold), -1)


def _decode_source(data: bytes) -> tuple[
        dict[str, int], list[tuple[bytes, bytes]], list[tuple[int, ...]]]:
    _require(len(data) >= SOURCE_HEADER.size, "source short header")
    fields = SOURCE_HEADER.unpack_from(data)
    (magic, version, header_size, frame_size, interval_size, frame_count,
     interval_count, fixed_delta_us, flags, session_id, generation, reserved0,
     reserved) = fields
    bundle_identity = ((version == 2 and flags == 0x0F) or
                       (version == 3 and flags == 0x1F))
    _require(magic == SOURCE_MAGIC and bundle_identity and
             header_size == SOURCE_HEADER.size and
             frame_size == SOURCE_FRAME.size and
             interval_size == SOURCE_INTERVAL.size and frame_count > 0 and
             interval_count > 0 and fixed_delta_us == 16667 and
             session_id != 0 and generation != 0 and reserved0 == 0 and
             reserved == bytes(8), "source header identity")
    _require(len(data) == SOURCE_HEADER.size + frame_count * SOURCE_FRAME.size +
             interval_count * SOURCE_INTERVAL.size, "source exact size")
    targets: list[tuple[bytes, bytes]] = []
    offset = SOURCE_HEADER.size
    for index in range(frame_count):
        frame = SOURCE_FRAME.unpack_from(data, offset)
        offset += SOURCE_FRAME.size
        tick, monotonic_ns = frame[0], frame[1]
        transform, linear = frame[14], frame[15]
        _require(tick == index and monotonic_ns == index * fixed_delta_us * 1000 and
                 _finite(transform) and _finite(linear) and
                 frame[16] == 0x7 and frame[17] == 0,
                 f"source frame {index}")
        targets.append((transform, linear))
    interval_profiles: list[list[int]] = [[] for _ in range(frame_count)]
    for index in range(interval_count):
        tick, ordinal, output_bits = SOURCE_INTERVAL.unpack_from(data, offset)
        offset += SOURCE_INTERVAL.size
        output = struct.unpack("<f", struct.pack("<I", output_bits))[0]
        _require(tick < frame_count and ordinal == len(interval_profiles[tick]) and
                 math.isfinite(output) and 0.0 < output <= 1.0,
                 f"source interval {index}")
        interval_profiles[tick].append(output_bits)
    _require(all(profile for profile in interval_profiles),
             "source interval coverage")
    return ({"frame_count": frame_count,
             "fixed_delta_us": fixed_delta_us,
             "interval_count": interval_count}, targets,
            [tuple(profile) for profile in interval_profiles])


def _decode_tick_receipts(text: str, frame_count: int) -> list[dict[str, int]]:
    reader = csv.DictReader(io.StringIO(text), strict=True)
    _require(tuple(reader.fieldnames or ()) == TICK_RECEIPT_COLUMNS,
             "tick receipt columns")
    receipts: list[dict[str, int]] = []
    for index, row in enumerate(reader):
        _require(index < frame_count and None not in row and
                 all(row[column] is not None for column in TICK_RECEIPT_COLUMNS),
                 f"tick receipt row {index}")
        try:
            receipt = {column: int(row[column], 10)
                       for column in TICK_RECEIPT_COLUMNS}
        except (TypeError, ValueError) as error:
            raise ValueError(f"tick receipt integer {index}") from error
        _require(receipt["tick"] == index and
                 receipt["physics_interval_calls"] > 0 and
                 all(receipt[column] >= 0 for column in TICK_RECEIPT_COLUMNS),
                 f"tick receipt identity {index}")
        receipts.append(receipt)
    _require(len(receipts) == frame_count, "tick receipt count")
    return receipts


def analyze(diagnostic: bytes, source: bytes,
            tick_receipts: str | None = None) -> dict[str, object]:
    source_info, targets, interval_profiles = _decode_source(source)
    receipts = (_decode_tick_receipts(tick_receipts,
                                      source_info["frame_count"])
                if tick_receipts is not None else None)
    _require(len(diagnostic) >= DIAGNOSTIC_HEADER.size,
             "diagnostic short header")
    fields = DIAGNOSTIC_HEADER.unpack_from(diagnostic)
    (magic, version, header_size, record_size, frame_count, fixed_delta_us,
     flags, session_id, generation, reserved0, source_sha256, reserved) = fields
    _require(magic == DIAGNOSTIC_MAGIC and version == 1 and
             header_size == DIAGNOSTIC_HEADER.size and
             record_size == DIAGNOSTIC_RECORD.size and frame_count > 0 and
             frame_count == source_info["frame_count"] and
             fixed_delta_us == source_info["fixed_delta_us"] and
             flags == DIAGNOSTIC_FLAGS and session_id != 0 and generation != 0 and
             reserved0 == 0 and reserved == bytes(16),
             "diagnostic header identity")
    _require(source_sha256 == hashlib.sha256(source).digest(),
             "diagnostic source SHA-256 mismatch")
    _require(len(diagnostic) == DIAGNOSTIC_HEADER.size +
             frame_count * DIAGNOSTIC_RECORD.size,
             "diagnostic exact size")

    transform_mismatch = 0
    linear_mismatch = 0
    raw_transform_mismatch = 0
    raw_linear_mismatch = 0
    raw_byte_only = 0
    corrected: list[bool] = []
    position_errors: list[float] = []
    basis_errors: list[float] = []
    linear_errors: list[float] = []
    first_divergence = -1
    first_transform_divergence = -1
    first_linear_divergence = -1
    top_frames: list[dict[str, float | int]] = []
    nearest = {"previous": 0, "current": 0, "next": 0}
    nearest_ties = 0
    early_divergences: list[dict[str, object]] = []
    target_positions = [tuple(_floats(transform)[12:15])
                        for transform, _linear in targets]
    target_states = [tuple(_floats(transform) + _floats(linear))
                     for transform, linear in targets]
    exact_full_state_neighbor = {"previous": 0, "current": 0, "next": 0,
                                 "none": 0}
    nearest_full_state_neighbor = {"previous": 0, "current": 0, "next": 0}
    nearest_full_state_ties = 0
    nearest_full_state_by_frame: list[str] = []
    offset = DIAGNOSTIC_HEADER.size
    for index, (target_transform_raw, target_linear_raw) in enumerate(targets):
        (tick, record_flags, reserved0, natural_transform_raw,
         natural_linear_raw, reserved) = DIAGNOSTIC_RECORD.unpack_from(
             diagnostic, offset)
        offset += DIAGNOSTIC_RECORD.size
        _require(tick == index and reserved0 == 0 and reserved == bytes(4) and
                 record_flags in (FLAG_EQUAL, FLAG_CORRECTED) and
                 _finite(natural_transform_raw) and _finite(natural_linear_raw),
                 f"diagnostic record {index}")
        raw_transform_differs = natural_transform_raw != target_transform_raw
        raw_linear_differs = natural_linear_raw != target_linear_raw
        transform_differs = not _component_equal_raw(
            natural_transform_raw, target_transform_raw)
        linear_differs = not _component_equal_raw(
            natural_linear_raw, target_linear_raw)
        differs = transform_differs or linear_differs
        _require(differs == bool(record_flags & FLAG_CORRECTED),
                 f"diagnostic class {index}")
        if differs and first_divergence < 0:
            first_divergence = index
        if transform_differs and first_transform_divergence < 0:
            first_transform_divergence = index
        if linear_differs and first_linear_divergence < 0:
            first_linear_divergence = index
        transform_mismatch += int(transform_differs)
        linear_mismatch += int(linear_differs)
        raw_transform_mismatch += int(raw_transform_differs)
        raw_linear_mismatch += int(raw_linear_differs)
        raw_byte_only += int(
            (raw_transform_differs or raw_linear_differs) and not differs)
        corrected.append(differs)

        natural_transform = _floats(natural_transform_raw)
        target_transform = _floats(target_transform_raw)
        natural_linear = _floats(natural_linear_raw)
        target_linear = _floats(target_linear_raw)
        position_error = _norm([
            natural_transform[i] - target_transform[i] for i in range(12, 15)
        ])
        basis_error = _norm([
            natural_transform[i] - target_transform[i] for i in range(12)
        ])
        linear_error = _norm([
            natural_linear[i] - target_linear[i] for i in range(3)
        ])
        position_errors.append(position_error)
        basis_errors.append(basis_error)
        linear_errors.append(linear_error)
        top_frames.append({"frame": index, "position_error": position_error,
                           "basis_error": basis_error,
                           "linear_error": linear_error})

        natural_state = tuple(natural_transform + natural_linear)
        state_candidates: list[tuple[str, int, float]] = []
        exact_labels: list[str] = []
        for label, target_index in (("previous", index - 1),
                                    ("current", index),
                                    ("next", index + 1)):
            if 0 <= target_index < len(target_states):
                target_state = target_states[target_index]
                distance = _norm([
                    natural_state[component] - target_state[component]
                    for component in range(19)
                ])
                state_candidates.append((label, target_index, distance))
                if all(natural_state[component] == target_state[component]
                       for component in range(19)):
                    exact_labels.append(label)
        if exact_labels:
            exact_full_state_neighbor[exact_labels[0]] += 1
        else:
            exact_full_state_neighbor["none"] += 1
        minimum_state_distance = min(
            distance for _label, _target_index, distance in state_candidates)
        nearest_state_labels = [
            label for label, _target_index, distance in state_candidates
            if math.isclose(distance, minimum_state_distance, rel_tol=1e-9,
                            abs_tol=1e-12)
        ]
        if len(nearest_state_labels) == 1:
            nearest_full_state_neighbor[nearest_state_labels[0]] += 1
            nearest_full_state_by_frame.append(nearest_state_labels[0])
        else:
            nearest_full_state_ties += 1
            nearest_full_state_by_frame.append("tie")
        if differs and len(early_divergences) < 16:
            early = {
                "frame": index,
                "transform_components": [i for i in range(16)
                                         if natural_transform[i] !=
                                         target_transform[i]],
                "linear_components": [i for i in range(3)
                                      if natural_linear[i] != target_linear[i]],
                "position_error": position_error,
                "basis_error": basis_error,
                "linear_error": linear_error,
                "exact_full_state_neighbor": exact_labels[0]
                if exact_labels else "none",
                "nearest_full_state_neighbor": nearest_state_labels[0]
                if len(nearest_state_labels) == 1 else "tie",
                "nearest_full_state_distance": minimum_state_distance,
            }
            if receipts is not None:
                source_calls = len(interval_profiles[index])
                replay_calls = receipts[index]["physics_interval_calls"]
                early.update({
                    "source_interval_calls": source_calls,
                    "replay_interval_calls": replay_calls,
                    "interval_call_delta": replay_calls - source_calls,
                })
            early_divergences.append(early)

        candidates: list[tuple[str, float]] = []
        for label, target_index in (("previous", index - 1),
                                    ("current", index),
                                    ("next", index + 1)):
            if 0 <= target_index < len(target_positions):
                distance = _norm([
                    natural_transform[12 + i] - target_positions[target_index][i]
                    for i in range(3)
                ])
                candidates.append((label, distance))
        minimum = min(distance for _label, distance in candidates)
        winners = [label for label, distance in candidates
                   if math.isclose(distance, minimum, rel_tol=1e-9,
                                   abs_tol=1e-12)]
        if len(winners) == 1:
            nearest[winners[0]] += 1
        else:
            nearest_ties += 1

    corrected_positions = [value for value, is_corrected in
                           zip(position_errors, corrected) if is_corrected]
    corrected_basis = [value for value, is_corrected in
                       zip(basis_errors, corrected) if is_corrected]
    corrected_linear = [value for value, is_corrected in
                        zip(linear_errors, corrected) if is_corrected]
    top_frames.sort(key=lambda item: (-float(item["position_error"]),
                                      int(item["frame"])))

    interval_calls_by_bits: dict[str, int] = {}
    interval_frames_by_bits: dict[str, int] = {}
    interval_group_indices: dict[str, list[int]] = {}
    for index, profile in enumerate(interval_profiles):
        unique_bits = tuple(dict.fromkeys(profile))
        signature = "+".join(f"0x{bits:08x}" for bits in unique_bits)
        interval_group_indices.setdefault(signature, []).append(index)
        for bits in set(profile):
            key = f"0x{bits:08x}"
            interval_frames_by_bits[key] = interval_frames_by_bits.get(key, 0) + 1
        for bits in profile:
            key = f"0x{bits:08x}"
            interval_calls_by_bits[key] = interval_calls_by_bits.get(key, 0) + 1
    interval_error_groups: dict[str, dict[str, object]] = {}
    for signature, indices in sorted(interval_group_indices.items()):
        interval_error_groups[signature] = {
            "frames": len(indices),
            "corrected_frames": sum(corrected[index] for index in indices),
            "position_error": _percentiles(
                [position_errors[index] for index in indices]),
            "linear_velocity_error": _percentiles(
                [linear_errors[index] for index in indices]),
            "linear_error_over_0_5": sum(
                linear_errors[index] > 0.5 for index in indices),
        }
    receipt_delta_groups: dict[str, dict[str, object]] = {}
    if receipts is not None:
        delta_indices: dict[int, list[int]] = {}
        for index, receipt in enumerate(receipts):
            delta = (receipt["physics_interval_calls"] -
                     len(interval_profiles[index]))
            delta_indices.setdefault(delta, []).append(index)
        for delta, indices in sorted(delta_indices.items()):
            nearest_counts = {"previous": 0, "current": 0, "next": 0,
                              "tie": 0}
            for index in indices:
                nearest_counts[nearest_full_state_by_frame[index]] += 1
            receipt_delta_groups[f"{delta:+d}"] = {
                "frames": len(indices),
                "corrected_frames": sum(corrected[index] for index in indices),
                "nearest_full_state": nearest_counts,
                "position_error": _percentiles(
                    [position_errors[index] for index in indices]),
                "linear_velocity_error": _percentiles(
                    [linear_errors[index] for index in indices]),
                "linear_error_over_0_5": sum(
                    linear_errors[index] > 0.5 for index in indices),
            }
    return {
        "format": "A9G5D1-first-divergence-v1",
        "frames": frame_count,
        "source_sha256": source_sha256.hex(),
        "equal_frames": frame_count - sum(corrected),
        "corrected_frames": sum(corrected),
        "first_divergence_frame": first_divergence,
        "first_transform_divergence_frame": first_transform_divergence,
        "first_linear_divergence_frame": first_linear_divergence,
        "transform_mismatch_frames": transform_mismatch,
        "linear_mismatch_frames": linear_mismatch,
        "raw_transform_mismatch_frames": raw_transform_mismatch,
        "raw_linear_mismatch_frames": raw_linear_mismatch,
        "raw_byte_only_frames": raw_byte_only,
        "longest_consecutive_correction_run": _longest_true_run(corrected),
        "longest_correction_runs": _true_runs(corrected)[:10],
        "early_divergent_frames": early_divergences,
        "first_position_error_over": {
            "1e-6": _first_over(position_errors, 1e-6),
            "1e-4": _first_over(position_errors, 1e-4),
            "1e-2": _first_over(position_errors, 1e-2),
            "1e-1": _first_over(position_errors, 1e-1),
            "5e-1": _first_over(position_errors, 5e-1),
        },
        "first_linear_error_over": {
            "1e-6": _first_over(linear_errors, 1e-6),
            "1e-4": _first_over(linear_errors, 1e-4),
            "1e-2": _first_over(linear_errors, 1e-2),
            "1e-1": _first_over(linear_errors, 1e-1),
            "5e-1": _first_over(linear_errors, 5e-1),
        },
        "position_error_corrected": _percentiles(corrected_positions),
        "basis_error_corrected": _percentiles(corrected_basis),
        "linear_velocity_error_corrected": _percentiles(corrected_linear),
        "nearest_position_target": nearest,
        "nearest_position_target_ties": nearest_ties,
        "exact_full_state_neighbor": exact_full_state_neighbor,
        "nearest_full_state_neighbor": nearest_full_state_neighbor,
        "nearest_full_state_neighbor_ties": nearest_full_state_ties,
        "top_position_error_frames": top_frames[:10],
        "source_interval_calls_by_bits": interval_calls_by_bits,
        "source_interval_frames_by_bits": interval_frames_by_bits,
        "source_interval_error_groups": interval_error_groups,
        "tick_receipts_present": receipts is not None,
        "tick_receipt_totals": ({
            column: sum(receipt[column] for receipt in receipts)
            for column in TICK_RECEIPT_COLUMNS if column != "tick"
        } if receipts is not None else {}),
        "interval_call_delta_groups": receipt_delta_groups,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("diagnostic", type=pathlib.Path)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--tick-receipts", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        receipt_text = (args.tick_receipts.read_text(encoding="utf-8")
                        if args.tick_receipts is not None else None)
        result = analyze(args.diagnostic.read_bytes(), args.source.read_bytes(),
                         receipt_text)
        encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.output is None:
            print(encoded, end="")
        else:
            args.output.write_text(encoded, encoding="utf-8")
        print(
            "G5_REPLAY_DIVERGENCE_VALID passed=1 "
            f"frames={result['frames']} equal={result['equal_frames']} "
            f"corrected={result['corrected_frames']} "
            f"first={result['first_divergence_frame']} "
            f"first_transform={result['first_transform_divergence_frame']} "
            f"first_linear={result['first_linear_divergence_frame']}"
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"g5_replay_divergence_error={error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
