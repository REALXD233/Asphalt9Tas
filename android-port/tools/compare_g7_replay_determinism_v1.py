#!/usr/bin/env python3
"""Compare source-bound G4/G5/G6 replay runs without adding gameplay hooks."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys
from itertools import combinations

import analyze_g5_replay_divergence_v1 as g5


STATUS_REQUIRED = {
    "complete": "1",
    "error": "0",
    "reject": "0",
    "control": "0,1",
}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _status_field(text: str, name: str) -> str:
    match = re.search(rf"(?:^|\s){re.escape(name)}=([^\s]+)", text)
    _require(match is not None, f"status missing {name}")
    return match.group(1)


def _decode_source_actions(source: bytes) -> tuple[dict[str, int], list[int]]:
    info, _targets, _intervals = g5._decode_source(source)
    nitro: list[int] = []
    accelerator_nonzero = 0
    offset = g5.SOURCE_HEADER.size
    for index in range(info["frame_count"]):
        frame = g5.SOURCE_FRAME.unpack_from(source, offset)
        offset += g5.SOURCE_FRAME.size
        accelerator_nonzero += int(frame[4] != 0.0)
        nitro.append(frame[5])
        _require(frame[5] <= 8, f"source nitro count {index}")
    return ({"accelerator_nonzero_frames": accelerator_nonzero,
             "nitro_calls": sum(nitro)}, nitro)


def _decode_natural_states(diagnostic: bytes) -> tuple[list[bytes], list[bool]]:
    fields = g5.DIAGNOSTIC_HEADER.unpack_from(diagnostic)
    frame_count = fields[4]
    states: list[bytes] = []
    corrected: list[bool] = []
    offset = g5.DIAGNOSTIC_HEADER.size
    for _index in range(frame_count):
        (_tick, flags, _reserved0, transform, linear,
         _reserved) = g5.DIAGNOSTIC_RECORD.unpack_from(diagnostic, offset)
        offset += g5.DIAGNOSTIC_RECORD.size
        states.append(transform + linear)
        corrected.append(flags == g5.FLAG_CORRECTED)
    return states, corrected


def _longest_false_run(values: list[bool]) -> int:
    longest = 0
    current = 0
    for value in values:
        current = 0 if value else current + 1
        longest = max(longest, current)
    return longest


def _validate_status(status: str, restore: str, frames: int,
                     analysis: dict[str, object]) -> dict[str, int]:
    for name, expected in STATUS_REQUIRED.items():
        _require(_status_field(status, name) == expected,
                 f"status {name}")
    for name in ("ticks", "begin", "interval", "final", "end"):
        _require(int(_status_field(status, name)) == frames,
                 f"status {name}")
    checks = _status_field(status, "checks").split(",")
    _require(len(checks) == 6 and all(value == "1" for value in checks),
             "status checks")
    physics = tuple(int(value) for value in
                    _status_field(status, "physics").split(","))
    _require(len(physics) == 4 and
             physics[0] == analysis["equal_frames"] and
             physics[1] == analysis["corrected_frames"] and
             physics[0] + physics[1] == frames and
             physics[2] == 2 * physics[1] and physics[3] == 0,
             "status physics receipt")
    _require(_status_field(restore, "action") == "3" and
             _status_field(restore, "status") == "3" and
             int(_status_field(restore, "ticks")) == frames and
             _status_field(restore, "detached") == "1",
             "restore receipt")
    return {"natural_equal_frames": physics[0],
            "corrected_frames": physics[1],
            "correction_writes": physics[2]}


def _load_run(source: bytes, source_sha: str, source_nitro: list[int],
              run_dir: pathlib.Path) -> dict[str, object]:
    diagnostic_path = run_dir / "natural-before-correction.a9g5d1"
    receipts_path = run_dir / "tick-receipts.csv"
    status_path = run_dir / "status.txt"
    restore_path = run_dir / "restore.txt"
    for path in (diagnostic_path, receipts_path, status_path, restore_path):
        _require(path.is_file(), f"missing {path}")
    diagnostic = diagnostic_path.read_bytes()
    receipt_text = receipts_path.read_text(encoding="utf-8")
    analysis = g5.analyze(diagnostic, source, receipt_text)
    _require(analysis["source_sha256"] == source_sha,
             "analysis source identity")
    frames = int(analysis["frames"])
    receipts = g5._decode_tick_receipts(receipt_text, frames)
    _require(all(receipt["steering_calls"] == 1 and
                 receipt["brake_calls"] == 1 and
                 receipt["accelerator_calls"] == 0 and
                 receipt["natural_nitro_calls"] == 0 and
                 receipt["suppressed_nitro_calls"] == 0 and
                 receipt["injected_nitro_calls"] == source_nitro[index]
                 for index, receipt in enumerate(receipts)),
             "per-tick action receipt")
    status_receipt = _validate_status(
        status_path.read_text(encoding="utf-8"),
        restore_path.read_text(encoding="utf-8"), frames, analysis)
    states, corrected = _decode_natural_states(diagnostic)
    _require(sum(corrected) == analysis["corrected_frames"],
             "diagnostic correction count")
    return {
        "name": run_dir.name,
        "path": str(run_dir.resolve()),
        "analysis": analysis,
        "receipts": receipts,
        "states": states,
        "status_receipt": status_receipt,
    }


def compare(source_path: pathlib.Path,
            run_dirs: list[pathlib.Path]) -> dict[str, object]:
    _require(len(run_dirs) >= 2, "at least two replay runs required")
    source = source_path.read_bytes()
    source_sha = hashlib.sha256(source).hexdigest()
    source_info, source_nitro = _decode_source_actions(source)
    _require(source_info["accelerator_nonzero_frames"] == 0,
             "current G7 scope excludes nonzero Accelerator sources")
    runs = [_load_run(source, source_sha, source_nitro, path)
            for path in run_dirs]
    frames = int(runs[0]["analysis"]["frames"])
    _require(all(int(run["analysis"]["frames"]) == frames for run in runs),
             "run frame count")

    pairwise: list[dict[str, object]] = []
    for left, right in combinations(runs, 2):
        state_equal = [
            g5._component_equal_raw(lhs, rhs)
            for lhs, rhs in zip(left["states"], right["states"])
        ]
        action_columns = tuple(
            column for column in g5.TICK_RECEIPT_COLUMNS
            if column not in ("tick", "physics_interval_calls")
        )
        action_equal = [
            all(left["receipts"][index][column] ==
                right["receipts"][index][column]
                for column in action_columns)
            for index in range(frames)
        ]
        interval_equal = [
            left["receipts"][index]["physics_interval_calls"] ==
            right["receipts"][index]["physics_interval_calls"]
            for index in range(frames)
        ]
        pairwise.append({
            "left": left["name"],
            "right": right["name"],
            "natural_state_equal_frames": sum(state_equal),
            "natural_state_different_frames": frames - sum(state_equal),
            "first_natural_state_difference": next(
                (i for i, equal in enumerate(state_equal) if not equal), -1),
            "longest_natural_state_difference_run":
                _longest_false_run(state_equal),
            "action_receipt_equal_frames": sum(action_equal),
            "physics_interval_count_equal_frames": sum(interval_equal),
            "first_physics_interval_count_difference": next(
                (i for i, equal in enumerate(interval_equal) if not equal), -1),
        })

    public_runs = []
    for run in runs:
        analysis = run["analysis"]
        public_runs.append({
            "name": run["name"],
            "path": run["path"],
            "equal_frames": analysis["equal_frames"],
            "corrected_frames": analysis["corrected_frames"],
            "first_divergence_frame": analysis["first_divergence_frame"],
            "longest_consecutive_correction_run":
                analysis["longest_consecutive_correction_run"],
            "physics_interval_calls":
                analysis["tick_receipt_totals"]["physics_interval_calls"],
            "injected_nitro_calls":
                analysis["tick_receipt_totals"]["injected_nitro_calls"],
            "status_receipt": run["status_receipt"],
        })

    natural_bit_exact = all(
        pair["natural_state_different_frames"] == 0 for pair in pairwise)
    action_exact = all(
        pair["action_receipt_equal_frames"] == frames for pair in pairwise)
    return {
        "format": "A9G7-multi-replay-determinism-v1",
        "source": str(source_path.resolve()),
        "source_sha256": source_sha,
        "frames": frames,
        "runs": public_runs,
        "pairwise": pairwise,
        "verdict": {
            "source_bound_runtime_receipts": True,
            "per_tick_action_receipts_deterministic": action_exact,
            "authoritative_final_state_receipts": True,
            "alutasv2_replay_semantics_proven": action_exact,
            "natural_pre_correction_bit_exact": natural_bit_exact,
            "strict_full_determinism_proven":
                action_exact and natural_bit_exact,
        },
        "scope": {
            "accelerator": "user-excluded; source required zero",
            "camera": "not serialized by upstream ReplayInput",
            "natural_state": "after-original and before conditional correction",
            "final_state": "validated runtime correction/readback receipt",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("runs", type=pathlib.Path, nargs="+")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--require-natural-bit-exact", action="store_true")
    args = parser.parse_args()
    try:
        result = compare(args.source, args.runs)
        encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.output is None:
            print(encoded, end="")
        else:
            args.output.write_text(encoded, encoding="utf-8")
        verdict = result["verdict"]
        print(
            "G7_REPLAY_DETERMINISM_VALID passed=1 "
            f"runs={len(result['runs'])} frames={result['frames']} "
            f"actions={int(verdict['per_tick_action_receipts_deterministic'])} "
            f"final_receipts={int(verdict['authoritative_final_state_receipts'])} "
            f"alutasv2_semantics={int(verdict['alutasv2_replay_semantics_proven'])} "
            f"natural_bit_exact={int(verdict['natural_pre_correction_bit_exact'])}"
        )
        if (args.require_natural_bit_exact and
                not verdict["natural_pre_correction_bit_exact"]):
            return 2
    except (OSError, ValueError, struct.error) as error:
        print(f"g7_replay_determinism_error={error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
