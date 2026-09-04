#!/usr/bin/env python3
"""Offline policy proof for final-writer preload and one-shot live linking."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPOSER = ROOT / "src" / "hwbp_final_writer_unified_replay_v1.cpp"
BOOTSTRAP = ROOT / "src" / "bootstrap_final_writer_replay_v1_build.cpp"
HELPER = ROOT / "tools" / "run_final_writer_preload_v1.sh"
BUILD = ROOT / "build-final-writer-live-gate-v1.ps1"
CANDIDATE = (ROOT / "build" / "final-writer-live-gate-v1" /
             "a9tas_final_writer_live_candidate_v1")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    composer = COMPOSER.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    helper = HELPER.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for needle in (
        "ReadProcessStartTicks", "expected_start_ticks",
        "A9TAS_FINAL_WRITER_READ_ONLY_GATE_V1",
        "FINAL_WRITER_READ_ONLY_COMPLETE",
        "PrearmFinalWriterUntilResume", "READY_NO_ATTACH_FINAL_WRITER_V1",
        "target_threads_attached=0 gameplay_writes=0",
        "payload_mapped=1 host_resume_gate=marker_removal",
        "O_RDONLY | O_CLOEXEC", "final_writer_replay_elf_v1::Resolve",
        "paused-zero baseline", "FINAL_WRITER_PAUSED_BASELINE",
        "READY_ARMED_FINAL_WRITER_V1", "all_target_threads_frozen=1",
        "CreateFinalWriterArmedMarker", "WaitForFinalWriterMarkerRemoval",
        "FINAL_WRITER_PAUSED_BASELINE",
    ):
        require(needle in composer, f"composer prearm policy missing: {needle}")
    require(composer.count("ReadProcessStartTicks(pid, &observed_start_ticks)") >= 2,
            "PID start time must be checked before READY and before attach")
    require("liba9tas_final_writer_replay_v1_build_only.so" in bootstrap,
            "bootstrap payload path mismatch")
    require("A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD" not in bootstrap,
            "final-writer bootstrap must retain safe default late-load policy")
    for needle in (
        "RC_RIP_BIAS=2", "--wait-window",
        "com.aligames.kuang.kybc.aligames", "/system/lib64/libnb.so",
        "libAsphalt9.so", "liba9tas_bootstrap_final_writer_v1.so",
    ):
        require(needle in helper, f"preload helper policy missing: {needle}")
    for needle in (
        "[switch]$EmitOneShotLiveCandidate",
        "$ExpectedReviewObjectSha256",
        "Explicit review-object SHA-256 acknowledgement mismatch",
        "controller=not_emitted", "controller=one_shot_local_only",
        "Remove-Item -LiteralPath $candidate -Force",
    ):
        require(needle in build, f"one-shot build policy missing: {needle}")
    require("adb" not in build.lower(), "offline build script accesses a device")
    require(not CANDIDATE.exists(),
            "default/offline state retained a live candidate")
    print("FINAL_WRITER_LIVE_GATE_POLICY passed=1 preload_window=1 "
          "pid_start_time_bound=1 ready_no_attach=1 ready_armed=1 one_shot_link=1 "
          "candidate_retained=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
