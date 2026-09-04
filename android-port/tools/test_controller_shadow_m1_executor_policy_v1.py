#!/usr/bin/env python3

import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"M1_EXECUTOR_POLICY_FAIL {message}")


def default_delta_only_view(source: str) -> str:
    """Select the macro-disabled legacy M1 branch for its frozen policy."""
    target = "#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1"
    output: list[str] = []
    # (parent_active, is_target)
    stack: list[tuple[bool, bool]] = []
    active = True
    for line in source.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == target:
            stack.append((active, True))
            active = False
            continue
        if stripped.startswith(("#if ", "#ifdef ", "#ifndef ")):
            stack.append((active, False))
            if active:
                output.append(line)
            continue
        if stripped == "#else" and stack:
            parent_active, is_target = stack[-1]
            if is_target:
                active = parent_active
            elif active:
                output.append(line)
            continue
        if stripped == "#endif" and stack:
            parent_active, is_target = stack.pop()
            if not is_target and active:
                output.append(line)
            active = parent_active
            continue
        if active:
            output.append(line)
    require(not stack, "unterminated_preprocessor_block")
    return "".join(output)


def main() -> None:
    require(len(sys.argv) == 2, "usage")
    raw_source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    source = default_delta_only_view(raw_source)

    for forbidden in (
        "BoundaryDr7(",
        "InstallBeforeResume(",
        "InstallAtCertifiedPrefix(",
        "BeginTick(",
        "AcknowledgeAtCallbackClose(",
        "PTRACE_SINGLESTEP",
        "run_prephysics_nitro_rpc",
        "kC98Offset",
        "kC9COffset",
        "kWorldAccumulatorOffset",
    ):
        require(forbidden not in source, f"forbidden={forbidden}")

    require(source.count("AccumulatorOnlyDr7()") == 2,
            "delta_only_dr7_call_count")
    require("pid, delta_address, 0, 0, 0, threads, &failures" in source,
            "stopped_attach_not_delta_only")
    require("pid, delta_address, 0, 0, 0, &threads, &failures" in source,
            "live_attach_not_delta_only")

    writer_install = source.find("InstallFinalWriterFrozen(mem")
    controller_install = source.find("host_session::Install(")
    resume = source.find("ResumeAll(&threads")
    require(0 <= writer_install < controller_install < resume,
            "prearm_install_order")
    ready_write = source.find("WriteReadyMarker(argv[8]")
    ready_wait = source.find("WaitForReadyRemoval(argv[8]")
    require(controller_install < ready_write < ready_wait < resume,
            "frozen_ready_gate_order")
    require("armed_lifecycle.state != 2" in source,
            "ready_gate_not_countdown_bound")
    require("all_target_threads_frozen=1 delta_writes=0" in source,
            "ready_gate_proof_missing")
    require("host_resume_gate=marker_removal" in source,
            "ready_gate_release_contract_missing")
    require("controller_pid=%d" in source and "getpid()" in source,
            "ready_gate_tracer_identity_missing")

    loop_begin = source.find("while (!stop_loop")
    loop_end = source.find("\ncleanup:", loop_begin)
    require(loop_begin >= 0 and loop_end > loop_begin, "event_loop_bounds")
    loop = source[loop_begin:loop_end]
    completion_branch = loop.find(
        "delta_core::Action::kFreezeAndValidateCompletion")
    require(completion_branch > 0, "completion_branch_missing")
    steady_loop = loop[:completion_branch]
    require(loop.count("WriteBackend(") == 1,
            "per_frame_write_primitive_count")
    require("WriteBackend(&mem, delta_address, &decision.write_value" in loop,
            "per_frame_write_not_fixed_delta")
    for forbidden in (
        "steering_bits",
        "brake_bits",
        "native_pose_address",
        "native_linear_address",
        "action_mailbox",
        "FramePermit",
    ):
        require(forbidden not in steady_loop, f"event_loop_owns={forbidden}")

    validate = source.find("host_session::ValidateComplete(")
    cleanup = source.find("\ncleanup:")
    require(resume < validate < cleanup, "completion_before_cleanup")
    require("host_session::Cleanup(" in source,
            "mandatory_controller_cleanup_missing")
    require("lifecycle::ResolveCountdownObject(" in source,
            "countdown_lifecycle_gate_missing")
    require("host_session::ValidateFinalInFlight(" in source,
            "pre_next_tick_final_completion_missing")
    require("kPreNextTickCompletion" in source and
            "observed_delta > 0" in source and
            "state.fixed_delta_requests == report.frame_count" in source,
            "pre_next_tick_completion_proof_missing")
    require("final_writer::ConditionalRollback(" in source,
            "mandatory_writer_cleanup_missing")

    print(
        "M1_EXECUTOR_POLICY passed=1 hwbp_addresses=1 "
        "per_frame_writes=fixed_delta_only payload_order=writer_then_controller "
        "frozen_ready_gate=1 completion_pending_boundary=1 mandatory_cleanup=1"
    )


if __name__ == "__main__":
    main()
