#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import pathlib
import struct


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-final-writer-natural-action-composite-v1.ps1"
INTERVAL_SOURCE = (
    ROOT
    / "evidence"
    / "a9tas_composite_900f_20260823_105723_425.a9pgtr2.intervals.bin"
)
INTERVAL_SOURCE_SHA256 = (
    "329683a976ad7d0e3f7d326c567fbdfaf911be1fc6bca5573087de2e11bf6347"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    text = RUNNER.read_text(encoding="utf-8")
    interval_bytes = INTERVAL_SOURCE.read_bytes()
    require(len(interval_bytes) == 900 * 4,
            "exact Physics Interval source must contain 900 uint32 values")
    require(hashlib.sha256(interval_bytes).hexdigest() ==
            INTERVAL_SOURCE_SHA256,
            "exact Physics Interval source hash changed")
    interval_bits = struct.unpack("<900I", interval_bytes)
    require(interval_bits.count(0x3C888889) == 884,
            "expected 884 recorded 60 Hz interval values")
    require(interval_bits.count(0x3C088889) == 16,
            "expected 16 recorded 120 Hz interval values")
    require(set(interval_bits) == {0x3C888889, 0x3C088889},
            "unexpected value in exact Physics Interval source")
    non_60_runs: list[tuple[int, int]] = []
    index = 0
    while index < len(interval_bits):
        if interval_bits[index] == 0x3C888889:
            index += 1
            continue
        start = index
        while index < len(interval_bits) and interval_bits[index] == 0x3C088889:
            index += 1
        non_60_runs.append((start, index - 1))
    require(non_60_runs == [(728, 731), (734, 737), (837, 840),
                            (877, 880)],
            "recorded 120 Hz interval runs changed")
    for token in (
        '"OfflineValidate", "PrepareFreshProcess", "ExecuteComposite"',
        "AcknowledgeThreeAutomaticEscapeTransitions",
        "Acknowledge900FrameCompositeWrites",
        "EnablePhysicsIntervalRecord",
        "Acknowledge900PhysicsIntervalRecordCalls",
        "EnablePhysicsIntervalReplay",
        "PhysicsIntervalReplaySource",
        "Acknowledge900PhysicsIntervalReplayCalls",
        "Physics Interval record and replay modes are mutually exclusive",
        "a9tas_composite_900f_20260823_105723_425.a9pgtr2.intervals.bin",
        "329683a976ad7d0e3f7d326c567fbdfaf911be1fc6bca5573087de2e11bf6347",
        "a9tas_lifecycle_source_900f_20260822_121419_985.a9utk1",
        "a9tas_lifecycle_natural_900f_20260822_121419_985.a9fwt1",
        "liba9tas_bootstrap_final_writer_natural_action_physics_interval_v1.so",
        "run_final_writer_natural_action_physics_interval_preload_v1.sh",
        "liba9tas_physics_interval_getter_v2_passive.so",
        "a9tas_physics_interval_shadow_controller_v2",
        "a9tas_physics_interval_readonly_observer_v1",
        "physics_interval_record=900 triple_preload=1",
        "physics_interval_replay=900 source_sha256=$intervalReplayExpectedHash triple_preload=1",
        "frames=900 source=recorded_natural activation_frame=430 action_calls=1",
        "AcknowledgeFailureForceStopsFreshProcess",
        "NAL_EXTERNAL_REPLAY_READY",
        "published=0 completed=0",
        "input keyevent 111 && echo NAL_ARM",
        "re-pause after callback registration",
        "READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1",
        "rm -f $externalReady",
        "validate_natural_action_external_replay_report_v1.py",
        "validate_final_writer_report_pair_v1.py",
        "parse_physics_interval_shadow_receipt_v2.py",
        "PHYSICS_INTERVAL_SHADOW_PREFLIGHT",
        "PHYSICS_INTERVAL_SHADOW_STATUS complete=1",
        "$intervalMode $intervalLimit",
        "push exact Physics Interval replay source",
        "Physics Interval replay-source entrypoint preflight failed",
        "$intervalEvent.requested_bits -eq $intervalEntry.bits",
        "$intervalEvent.final_bits -eq $intervalEntry.bits",
        "evidence.tid_changes -ne 0",
        "evidence.replay_calls -ne $expectedReplayCalls",
        "evidence.overrides -ne $expectedOverrides",
        "replay receipt does not match the exact pinned source stream",
        "finalize $gamePid",
        "rollback $gamePid",
        "evidence.record_calls -ne $expectedRecordCalls",
        "events).Count -ne $intervalLimit",
        "am force-stop $package",
    ):
        require(token in text, f"runner gate missing: {token}")
    require(text.count("input keyevent 111") == 3,
            "runner must contain exactly three explicit ESC transitions")
    repause = text.index("re-pause after callback registration")
    interval_install = text.index("install Physics Interval $intervalMode hook")
    writer_start = text.index("$writerCommand =")
    require(repause < interval_install < writer_start,
            "Physics Interval recorder must install after registration re-pause "
            "and before final-writer start")
    require("Mode = \"OfflineValidate\"" in text,
            "runner does not default offline")
    print("RUN_FINAL_WRITER_NATURAL_ACTION_COMPOSITE_POLICY passed=1 "
          "offline_default=1 paired_preload=1 optional_triple_preload=1 "
          "three_ESC=1 dual_reports=1 interval_receipt=900 "
          "interval_replay_source_pinned=1 exact_override_receipt=1 "
          "interval_distribution=884x60hz+16x120hz "
          "force_stop_on_uncertain_failure=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
