#!/usr/bin/env python3
"""Static regression guard for the G8 actual-length recording seam."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(text: str, tokens: tuple[str, ...], label: str) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        raise SystemExit(f"G8 lifecycle policy missing in {label}: {missing}")


def main() -> int:
    protocol = (ROOT / "src/g4_multi_hook_runtime_v1.h").read_text()
    boundary = (ROOT / "src/g3_boundary_adapter_v1.h").read_text()
    bridge = (ROOT / "src/g4_g3_adapter_v1.h").read_text()
    payload = (ROOT / "src/payload_g4_multi_hook_runtime_v1.cpp").read_text()
    controller = (ROOT / "src/g4_input_action_controller_v1.cpp").read_text()
    runner = (ROOT / "run-g4-input-action-gate-v1.ps1").read_text()

    require(protocol, (
        "enum class CompletionPolicy", "kRaceLifecycle = 1",
        "enum class CompletionReason", "completion_policy",
        "completion_reason", "kHookCount = 8",
        "kRearmArchivedReplay = 4", "kRearmArchivedRecord = 5",
        "archived_generation",
        "archived_frame_count", "archived_interval_count",
        "kVersion = 13", "kMaximumIntervalSamples = 16384",
        "ArmFrameLimitValid",
    ), "protocol")
    require(boundary, (
        "kMaximumNeutralReceipts = 7200",
    ), "extended lifecycle capacity")
    require(bridge, (
        "ObserveRaceEndAtClosedBoundary", "state->input_action.tick_open",
        "Result::kRaceEnded", "DiscardOpenTickForRaceEnd",
        "RearmAfterArchivedRace", "ResetSessionMetadata",
    ), "existing G4/G3 bridge")
    require(payload, (
        "MaybeCompleteLifecycleRecording", "G4SubmitBeforeV1",
        "G4FrameAfterV1", "CompletionReason::kRaceLifecycle",
        "Fault(kErrorRecordingCapacity)",
        "g_runtime.receipt_count - 1u",
        "MaybeDiscardOpenLifecycleTick", "RestoreDiscardedTickSemantics",
        "ArchivedCompletedReceiptValid", "PublishedTargetsValid",
        "RearmArchivedRecord", "RearmAfterSealedCheckpointAtRetry",
        "bridge::RearmAfterArchivedRace", "ResetForArchivedRearm",
        "ArmFrameLimitValid(g_control.frame_limit, mode, completion",
    ), "payload")
    if payload.count("MaybeCompleteLifecycleRecording(lifecycle)") != 2:
        raise SystemExit("G8 payload must observe lifecycle at exactly the existing submit and frame boundaries")
    require(controller, (
        '"record-life"', "Action::kArmLifecycleRecord",
        '"wait"', "Action::kWaitForCompletion",
        "completed_frames = lifecycle_completion",
        "CompletionReason::kRaceLifecycle",
        "evidence.recorded_frames", "frame_count",
        "usleep(10000)", '"rearm-replay"', '"rearm-record"',
        "Action::kRearmArchivedRecord",
        "SealedLifecycleRecordForRearmValid",
        "CompletedResidentSessionForRearmValid",
        '"pause-probe"', "Action::kPauseProbe",
        '"wait-progress"', "Action::kWaitForReplayProgress",
        "kMinimumProgressTicks = 120",
        "ReplayProgressCursorValid", "replay_head == static_cast<std::size_t>(receipts) + 1u",
        "kPauseProbeDelayUs = 2000000",
        "std::memcmp(&before_state, &after_state, sizeof(before_state)) == 0",
        "ReadCompletionReceipt", "ReadCompletionBytes",
        "kPollCount = 180000", "kMaximumConsecutiveReadFailures = 1000",
        "G4_WAIT_READ failures=%u max_consecutive=%u",
        '"G8_PAUSE_PROBE passed=%d',
    ), "controller")
    require(runner, (
        '"ExecuteLifecycleRecordGate"', "'record-life'", "'wait'",
        "actualTicks", "completion=1,2",
        "G8_LIFECYCLE_RECORD_GATE_PASSED",
        '"ExecuteLifecycleRetryReplayGate"', "'rearm-replay'",
        "G8_ARCHIVE_READY_WAITING_FOR_PHASE2",
        "phase2.ready", "pause_retry_countdown3",
        "G8_LIFECYCLE_RETRY_REPLAY_GATE_PASSED",
        '"ExecutePauseResumeReplayGate"', "'pause-probe'",
        "'wait-progress'", "G8_PROGRESS_WAIT passed=1 minimum=120",
        "pause active replay once", "resume paused replay once",
        "G8_PAUSE_RESUME_REPLAY_GATE_PASSED",
        "[ValidateRange(1, 7200)]", "extended capacity is lifecycle-only",
        '$GamePackage = "com.aligames.kuang.kybc.aligames"',
        "$GameProcessName", "$GameActivity", "$TargetProcessId",
        "Resolve-GameActivity", "pidof',$processName",
        "nohup $remoteCarrier $processName",
        "Use -TargetProcessId", "package=$package process=$processName",
        "$retryObservation 0 $remoteBuildProfile",
        "--build-profile $selectedProfile.Path",
    ), "runner")

    forbidden = (
        "camera smoothing", "visual classifier",
        "per-frame ptrace",
    )
    present = [token for token in forbidden if token in payload]
    if present:
        raise SystemExit(f"G8 lifecycle seam introduced forbidden payload scope: {present}")

    print(
        "G8_LIFECYCLE_RECORD_POLICY passed=1 boundaries=submit,frame "
        "actual_length=1 read_only_wait=1 archived_rearm=1 "
        "strict_generation=1 retry_tick0=1 pause_resume_probe=1 "
        "retry_build_profile=1 "
        "extended_capacity=7200 constant_size_wait=1 "
        "physical_hooks=8 new_hooks=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
