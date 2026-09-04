#!/usr/bin/env python3
"""Offline policy for the disabled full native-ARM64 G4 controller."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
G4 = ROOT / "src/g4_input_action_controller_v1.cpp"
G2 = ROOT / "src/g2_physics_interval_controller_v1.cpp"
HABI = ROOT / "src/habi1_one_shot_controller.cpp"
COMMAND = ROOT / "src/native_arm64_command_backend_v1.cpp"
PAYLOAD = ROOT / "src/payload_g4_multi_hook_runtime_v1.cpp"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def cancelled_queue_predecessor_model(staged: dict, reason: str,
                                       coordinator_generation: int):
    valid = (
        staged["enabled"] == 0 and staged["completed"] == 1 and
        staged["active_helpers"] == 0 and staged["pending_mode"] == 0 and
        staged["pending_state"] == 0 and
        staged["pending_generation"] == 0 and
        staged["pending_reserved"] == 0 and
        0 < staged["archived_generation"] < 0xFFFFFFFF and
        staged["generation"] == staged["archived_generation"] + 1 and
        0 < staged["archived_frame_count"] <= 0xFD200 and
        coordinator_generation == staged["archived_generation"])
    if not valid:
        return None
    if reason in ("race", "checkpoint"):
        mode, policy = "record", "lifecycle"
    elif reason == "fixed":
        mode, policy = "replay", "fixed"
    else:
        return None
    return {
        "generation": staged["archived_generation"],
        "frame_limit": staged["archived_frame_count"],
        "interval_count": staged["archived_interval_count"],
        "mode": mode,
        "policy": policy,
    }


def main() -> int:
    g4 = G4.read_text("utf-8")
    g2 = G2.read_text("utf-8")
    habi = HABI.read_text("utf-8")
    command = COMMAND.read_text("utf-8")
    payload = PAYLOAD.read_text("utf-8")
    joined = "\n".join((g4, g2, habi, command, payload))

    for token in (
        "A9TAS_G4_NATIVE_ARM64_CONTROLLER",
        "native_arm64_g4_payload_resolver_v1::Resolve",
        "native_arm64_immutable_trap_resolver_v1::Resolve",
        "ReturnStopAddress(",
        "stopped_has_immutable_trap",
        "trap={};",
        'FailG4(pid,&frozen,false,"native_stopped_identity",6)',
        "native_arm64_command_backend_v1::InvokeStopped",
        "remote_command_call_contract_v1::Request",
        "native_arm64_remote_call_v1::GetRegisters(",
        "frozen->call_tid, &original",
        "report.contract_match",
        'std::strcmp(text, "record")',
        'std::strcmp(text, "replay")',
        'std::strcmp(text, "rearm-replay")',
        'std::strcmp(text, "rearm-record")',
        'std::strcmp(text, "wait-pause-rearm")',
        "kWaitForCompletionPauseAndRearm = 23",
        'execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent", "111"',
        "protocol::Command::kInstallPassive",
        "protocol::Command::kArmSession",
        "protocol::Command::kRestore",
        "protocol::Command::kRearmArchivedRecord",
        "protocol::Command::kRearmPausedReplayRecord",
        'std::strcmp(text, "wait-activation")',
        'std::strcmp(text, "queue-active-record")',
        "kWaitForPendingActivation = 26",
        "kQueueActiveRecordRetry = 27",
        "protocol::Command::kQueueActiveRecordRetry",
        "kPackageNamespace[] = \"dev.a9tas.android\"",
        "package_tail != suffix && *package_tail != '.'",
        "receipt.session_id != receipt_session_id",
        "receipt.generation != receipt_generation",
        "archived_receipt_generation",
        "replacing_cancelled_queued_session",
        "CancelledQueuedPredecessorControl(",
        "state.tick.coordinator.generation != staged.archived_generation",
        "cancelled_queue_predecessor.frame_limit",
        "cancelled_queue_receipt || current_receipt",
        "? existing.archived_frame_count",
        "? existing.archived_interval_count",
        "const bool completed_record_queue =",
        "!active_record_queue && !archived_queue && !completed_record_queue",
        '" missing=0x%" PRIx64 " completion=%u/%u mode=%u policy=%u"',
    ):
        require(token in joined, f"missing native G4 invariant: {token}")

    require(g4.index("FreezeStable(pid, call_tid, &frozen)") <
            g4.index("stopped_native_identity="),
            "native identity must be re-resolved after complete freeze")
    require(g4.index("stopped_native_identity=") <
            g4.index("g2::CallGuest("),
            "native identity must precede the command call")
    require("RemoteCallOnce(frozen->call_tid" in g2,
            "proven x86 command path was removed")
    require("#if defined(__x86_64__)" in habi,
            "x86 remote-call boundary is not architecture-scoped")
    config_begin = g4.index("} else if (IsSessionConfigurationAction(action))")
    config_end = g4.index("} else {", config_begin)
    config = g4[config_begin:config_end]
    require(config.index("ReadReceipt(mem, runtime, &existing") <
            config.index("ResolveInstallObjects(pid, resolution_owner"),
            "Retry rearm must read the sealed receipt before resolving new objects")
    for token in ("existing_evidence.last_object[protocol::kTickHook]",
                  "existing_evidence.last_vptr[protocol::kTickHook]",
                  'FailG4(pid, &frozen, false, "rearm_live_owner", 5)'):
        require(token in config, f"missing Retry live-owner binding: {token}")
    require(g4.count("existing_state, false)") >= 2,
            "archived rearm must validate sealed replay bytes without requiring the retired race object")
    cancelled_begin = g4.index("bool CancelledQueuedPredecessorControl(")
    cancelled_end = g4.index("bool RecordedBuffersValid(", cancelled_begin)
    cancelled = g4[cancelled_begin:cancelled_end]
    for token in (
            "staged.generation != staged.archived_generation + 1u",
            "predecessor->generation =",
            "staged.archived_generation",
            "predecessor->frame_limit =",
            "staged.archived_frame_count",
            "CompletionReason::kManualCheckpoint",
            "CompletionPolicy::kRaceLifecycle",
            "CompletionReason::kFixedFrameLimit",
            "CompletionPolicy::kFixedFrameLimit"):
        require(token in cancelled,
                f"cancelled queued predecessor reconstruction missing: {token}")
    require(g4.index("CancelledQueuedPredecessorControl(", config_begin) <
            g4.index("const bool cancelled_queue_receipt", config_begin),
            "cancelled queue must reconstruct its predecessor before receipt validation")
    require("require_recorded_native_identity &&" in g4,
            "ordinary replay validation lost its live native-object check")
    for token in ("BindPausedReplayRuntime(existing, &runtime)",
                  "current_interval == existing.expected_interval_owner",
                  'branch_rearm_action ? "branch_retained_objects"'):
        require(token in config,
                f"missing paused-replay retained binding: {token}")
    branch_bind = config.index("const bool objects_resolved = queued_rearm_action")
    countdown_scan = config.index(
        "ResolveInstallObjects(pid, resolution_owner, &runtime)", branch_bind)
    require(config.index("BindPausedReplayRuntime(existing, &runtime)",
                          branch_bind) < countdown_scan,
            "paused replay handoff must bind retained objects instead of "
            "requiring a countdown scan")
    for token in ("Action::kQueueArchivedRecord",
                  "Action::kQueueArchivedReplay",
                  "const bool objects_resolved = queued_rearm_action",
                  "protocol::Command::kQueueArchivedReplay",
                  "protocol::Command::kQueueArchivedRecord"):
        require(token in g4,
                f"resident lifecycle queue path missing: {token}")
    replay_publish = g4[g4.index("if (replay_action) {", config_begin):
                        g4.index("if (rearm_action) {", config_begin)]
    require("} else if (record_rearm_action) {" in replay_publish and
            "} else if (branch_rearm_action) {" not in replay_publish and
            "control.replay_frame_count = 0;" in replay_publish and
            "control.replay_interval_count = 0;" in replay_publish and
            "std::memset(control.recording_sha256, 0," in replay_publish,
            "every replay-to-record rearm must clear replay-only control metadata")
    for token in ("G4_PENDING_WAIT passed=%d",
                  "pending_activations == prior_activations + 1",
                  "active_retry_queue", "already_active_retry"):
        require(token in g4,
                f"resident activation receipt path missing: {token}")
    pending_wait = g4[g4.index(
        "if (action == Action::kWaitForPendingActivation) {"):
        g4.index("if (action == Action::kWaitForRetryCountdownAndPause) {")]
    active_retry_shape = pending_wait[pending_wait.index(
        "const bool active_retry_queue ="):
        pending_wait.index("const bool queued =")]
    common_activation = pending_wait[pending_wait.index(
        "const bool common_activation ="):
        pending_wait.index("const bool pending_transition =")]
    require("const bool queue_shape =" in pending_wait and
            "active_retry_queue = queue_shape" in active_retry_shape and
            "TargetsMatch" not in active_retry_shape and
            "active_helpers" not in active_retry_shape and
            "TargetsMatch(mem, observed_control, true)" in common_activation,
            "direct-Retry wait must tolerate the closing callback/stale old targets "
            "but validate freshly rebound targets before accepting activation")
    for token in ("bool QueueActiveRecordRetry()",
                  "g_control.generation + 1u",
                  "Direct-Retry recording is a resident loop"):
        require(token in payload,
                f"direct Retry resident loop missing: {token}")
    cancel_pending = payload[payload.index("bool CancelPending()"):
                             payload.index("bool ArmSession()")]
    for token in ("const bool completed_record_queue =",
                  "g_evidence.status == kComplete",
                  "g_control.pending_generation == g_control.generation + 1u",
                  "!active_record_queue && !archived_queue && !completed_record_queue"):
        require(token in cancel_pending,
                f"sealed direct-Retry queue cancellation missing: {token}")
    interval_begin = payload.index("G4IntervalBeforeV1(void* owner)")
    interval_end = payload.index("G4IntervalAfterV1", interval_begin)
    interval = payload[interval_begin:interval_end]
    for token in ("g_control.enabled, __ATOMIC_ACQUIRE) == 0",
                  "g_evidence.last_object[kTickHook]",
                  "g_evidence.last_vptr[kTickHook]", "__ATOMIC_RELEASE"):
        require(token in interval,
                f"retained session lacks passive live-owner observation: {token}")
    require("backend_enabled=1" not in joined,
            "offline controller source must not enable a backend")
    require('execl("/system/bin/input"' not in g4,
            "Android input shell script must not be execve'd directly")
    wait_begin = g4.index(
        "if (action == Action::kWaitForCompletion ||")
    freeze_begin = g4.index("const pid_t call_tid = UniqueSignalCatcher(pid);")
    require(wait_begin < freeze_begin and
            "Action::kWaitForCompletionPauseAndRearm" in
            g4[wait_begin:freeze_begin],
            "combined replay-to-record handoff must wait before the stopped rearm")
    readonly_begin = g4.index("const int mem = open(mem_path,")
    readonly_end = g4.index("O_CLOEXEC);", readonly_begin)
    require("kWaitForCompletionPauseAndRearm" not in
            g4[readonly_begin:readonly_end],
            "combined handoff needs a writable descriptor for its retained-runtime rearm")
    require("Action::kWaitForCompletionAndPause" not in
            g4[readonly_begin:readonly_end],
            "target inspection needs a writable descriptor to release its exact-tick barrier")

    staged = {
        "enabled": 0, "completed": 1, "active_helpers": 0,
        "pending_mode": 0, "pending_state": 0, "pending_generation": 0,
        "pending_reserved": 0, "generation": 12,
        "archived_generation": 11, "archived_frame_count": 591,
        "archived_interval_count": 616,
    }
    checkpoint = cancelled_queue_predecessor_model(staged, "checkpoint", 11)
    require(checkpoint == {
                "generation": 11, "frame_limit": 591,
                "interval_count": 616, "mode": "record",
                "policy": "lifecycle"},
            "cancelled queued checkpoint did not reconstruct its predecessor")
    race = cancelled_queue_predecessor_model(staged, "race", 11)
    require(race is not None and race["mode"] == "record",
            "cancelled queued race must remain a lifecycle recording")
    replay = cancelled_queue_predecessor_model(staged, "fixed", 11)
    require(replay is not None and replay["mode"] == "replay" and
            replay["frame_limit"] == 591 and replay["interval_count"] == 616,
            "cancelled queued replay lost archived counts")
    for mutation, reason, coordinator in (
            ({**staged, "pending_state": 1}, "checkpoint", 11),
            ({**staged, "generation": 13}, "checkpoint", 11),
            (staged, "checkpoint", 12),
            (staged, "unknown", 11)):
        require(cancelled_queue_predecessor_model(
                    mutation, reason, coordinator) is None,
                "invalid cancelled queue was accepted by the state model")
    wait_pause = g4[g4.index("bool target_pause_injected = false;"):
                    g4.index("if (action == Action::kWaitForCompletionPauseAndRearm &&")]
    for token in ("target_completion_barrier_released",
                  "offsetof(protocol::Control, replay_speed_reserved)",
                  "g2::WriteExact(mem, barrier_address, &disabled",
                  "observed == disabled"):
        require(token in wait_pause,
                f"target inspection barrier release is incomplete: {token}")
    dispatcher = payload[payload.index("G4DispatcherEntryV1(void* owner"):
                         payload.index("void LockBarrelPrng()")]
    require("g_control.replay_speed_reserved" in dispatcher and
            "kReplayCompletionBarrierEnabled" in dispatcher,
            "payload completion barrier cannot observe the host release request")
    frame_after = payload[payload.index("G4FrameAfterV1("):
                          payload.index("G4NitroBeforeV1(")]
    atomic_handoff = payload[payload.index(
        "bool CompleteReplayToAtomicRecording() {"):
        payload.index("bool RearmPausedReplayRecord()")]
    for token in ("recording_before_tick_end",
                  "CompleteReplayToAtomicRecording()"):
        require(token in frame_after or token in payload,
                f"FrameAfter atomic replay handoff missing: {token}")
    for token in ("StagedReplayPrefixValid(prefix_count)",
                  "SeedValidatedContinuousRecordingPrefix(prefix_count)",
                  "ContinueReplayAsRecordAtClosedBoundary",
                  "kReplayRecordHandoffComplete",
                  "g_control.frame_limit = kMaximumFrames",
                  "prior_generation == UINT32_MAX",
                  "g_control.pending_mode != 0u",
                  "g_control.pending_generation != 0u",
                  "g_control.pending_state, 0u"):
        require(token in atomic_handoff,
                f"atomic replay-to-record ownership switch incomplete: {token}")
    require(atomic_handoff.index(
                "ContinueReplayAsRecordAtClosedBoundary") <
            atomic_handoff.index(
                "SeedValidatedContinuousRecordingPrefix(prefix_count)") <
            atomic_handoff.index(
                "g_control.mode = static_cast<std::uint32_t>(RunMode::kRecord)"),
            "atomic replay handoff publishes Control before coordinator commit")
    require("StageAtomicReplayRecording" not in payload,
            "atomic replay handoff must not reuse lifecycle pending fields")
    for token in ("target_atomic_handoff",
                  "kReplayCompletionAtomicRecord",
                  "waiting_control.pending_mode == 0u",
                  "waiting_control.pending_generation == 0u",
                  "if (!queued_rearm_action)",
                  "control.pending_state = 0u",
                  "barrier=%u generation=%u pending=%u,%u,%u,%u",
                  '" prefix_ticks=%u detached=1 handoff=1 pause=1'):
        require(token in g4,
                f"controller atomic handoff receipt missing: {token}")
    for token in ("accelerated_generation",
                  "__atomic_load_n(&g_control.mode, __ATOMIC_ACQUIRE)",
                  "__atomic_load_n(&g_control.generation, __ATOMIC_ACQUIRE)",
                  "__atomic_load_n(&g_control.replay_speed_factor, __ATOMIC_ACQUIRE)"):
        require(token in dispatcher,
                f"accelerated replay can leak across a lifecycle generation switch: {token}")
    for token in ("kRecordCheckpointBarrierRequested",
                  "kRecordCheckpointBarrierHeld",
                  "checkpoint_receipts_before",
                  "g_runtime.receipt_count > checkpoint_receipts_before",
                  "coordinator::TickPhase::kClosed"):
        require(token in dispatcher,
                f"record checkpoint barrier is incomplete: {token}")
    checkpoint_controller = g4[g4.index(
        "if (action == Action::kCheckpointAtNextClosedTick) {"):
        g4.index("if (action == Action::kWaitForCompletion ||")]
    for token in ('"checkpoint-pause"',
                  "kRecordCheckpointBarrierRequested",
                  "kRecordCheckpointBarrierHeld",
                  "waiting_state.receipt_count > record_checkpoint_initial_ticks",
                  '"checkpoint_boundary_timeout"',
                  '"checkpoint_pause_delivery"',
                  '" checkpoint=%d checkpoint_ticks=%u,%u pause=%d release=%d'):
        require(token in g4 if token in (
                            '"checkpoint-pause"',
                            '" checkpoint=%d checkpoint_ticks=%u,%u pause=%d release=%d') else
                        token in checkpoint_controller,
                f"exact record checkpoint controller path missing: {token}")
    require('target_release=%d' in g4,
            "target inspection receipt does not prove barrier release")
    for token in ("kPollCount = 180000",
                  "kMaximumConsecutiveReadFailures = 1000",
                  "continue;"):
        require(token in wait_pause,
                f"completion waiter lacks full-lap/read-failure tolerance: {token}")
    restore_begin = payload.index("bool Restore()")
    restore_end = payload.index("std::uint64_t Return(", restore_begin)
    restore_body = payload[restore_begin:restore_end]
    for token in ("g_runtime.input_action.tick_open",
                  "bridge::DiscardOpenTickForRaceEnd",
                  "RestoreDiscardedTickSemantics(open_tick)",
                  "coordinator::TickPhase::kClosed"):
        require(token in restore_body,
                f"restore cannot normalize an unpublished open tick: {token}")
    restored_begin = g4.index("bool RestoredSessionReusableValid(")
    restored_end = g4.index("bool PassiveControlValid(", restored_begin)
    require("CompleteReceiptValid(" not in g4[restored_begin:restored_end],
            "restored reuse must mirror the payload contract after UI pause")
    print("NATIVE_ARM64_G4_CONTROLLER_POLICY passed=1 shared_g4_core=1 "
          "direct_payload=1 stopped_reresolve=1 shared_command_contract=1 "
          "x86_path_retained=1 cancelled_queue_matrix=7 backend_enabled=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as error:
        print(f"NATIVE_ARM64_G4_CONTROLLER_POLICY passed=0 error={error}")
        raise SystemExit(1)
