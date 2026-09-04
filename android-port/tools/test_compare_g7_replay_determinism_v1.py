#!/usr/bin/env python3
"""Policy tests for the offline G7 multi-replay comparator."""

from __future__ import annotations

import pathlib
import tempfile

import compare_g7_replay_determinism_v1 as g7


def main() -> int:
    source = pathlib.Path(
        "android-port/evidence/g4-900-record-20260825_083502_698/"
        "recording.a9g4r2")
    run_a = pathlib.Path(
        "android-port/evidence/g4-900-replay-20260825_084323_121")
    run_b = pathlib.Path(
        "android-port/evidence/g4-900-replay-20260825_084835_117")
    result = g7.compare(source, [run_a, run_b])
    verdict = result["verdict"]
    assert verdict["source_bound_runtime_receipts"]
    assert verdict["per_tick_action_receipts_deterministic"]
    assert verdict["authoritative_final_state_receipts"]
    assert verdict["alutasv2_replay_semantics_proven"]
    assert not verdict["natural_pre_correction_bit_exact"]
    assert not verdict["strict_full_determinism_proven"]
    assert result["pairwise"][0]["natural_state_different_frames"] > 0

    with tempfile.TemporaryDirectory() as temporary:
        bad = pathlib.Path(temporary) / "bad-run"
        bad.mkdir()
        for name in ("natural-before-correction.a9g5d1", "tick-receipts.csv",
                     "status.txt", "restore.txt"):
            (bad / name).write_bytes((run_a / name).read_bytes())
        status = (bad / "status.txt").read_text(encoding="utf-8")
        (bad / "status.txt").write_text(
            status.replace("error=0", "error=29"), encoding="utf-8")
        try:
            g7.compare(source, [run_a, bad])
        except ValueError as error:
            assert "status error" in str(error)
        else:
            raise AssertionError("faulted runtime receipt was accepted")

    print("COMPARE_G7_REPLAY_DETERMINISM_SELFTEST passed=1 cases=2")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
