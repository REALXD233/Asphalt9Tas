#!/usr/bin/env python3

import pathlib
import sys


TARGET = "#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"PHASE_PACED_EXECUTOR_POLICY_FAIL {message}")


def phase_view(source: str) -> str:
    output: list[str] = []
    # (parent_active, is_target, in_true_branch)
    stack: list[tuple[bool, bool, bool]] = []
    active = True
    for line in source.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == TARGET:
            stack.append((active, True, True))
            active = active
            continue
        if stripped.startswith(("#if ", "#ifdef ", "#ifndef ")):
            stack.append((active, False, True))
            if active:
                output.append(line)
            continue
        if stripped == "#else" and stack:
            parent_active, is_target, _ = stack[-1]
            if is_target:
                active = False
                stack[-1] = (parent_active, True, False)
            elif active:
                output.append(line)
            continue
        if stripped == "#endif" and stack:
            parent_active, is_target, _ = stack.pop()
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
    raw = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    source = phase_view(raw)

    for required in (
        "controller_shadow_phase_paced_core_v1.h",
        "phase_core::Observe(",
        "phase_core::Boundary::kWorldCommit",
        "BoundaryDr7(true)",
        "pid, delta_address, c98_address, c9c_address, world_accumulator",
        "host_session::ValidateFinalInFlight(",
        "state.committed_frames == report.frame_count",
        "per_frame_control_writes=0",
        "I_ACCEPT_PHASE_PACED_EXECUTOR_REVIEW_V1",
        "/data/local/tmp/a9tas_phase_ready_",
        "PHASE_PACED_READY_ARMED",
        "c98=0x%",
        "c9c=0x%",
        "world=0x%",
    ):
        require(required in source, f"missing={required}")
    require("delta_core::ObserveDelta(" not in source,
            "single_address_core_still_selected")

    loop_begin = source.find("phase_core::Config config")
    loop_end = source.find("\ncleanup:", loop_begin)
    require(loop_begin >= 0 and loop_end > loop_begin, "event_loop_bounds")
    loop = source[loop_begin:loop_end]
    require(loop.count("WriteBackend(") == 1,
            "per_frame_write_primitive_count")
    require("WriteBackend(&mem, delta_address, &decision.write_value" in loop,
            "per_frame_write_not_fixed_delta")
    for forbidden in (
        "ApplySteering(",
        "steering_bits",
        "brake_bits",
        "native_pose_address",
        "native_linear_address",
        "run_prephysics_nitro_rpc",
        "PTRACE_SINGLESTEP",
    ):
        require(forbidden not in loop, f"event_loop_owns={forbidden}")

    require("FreezeStablePhaseThreadSet(" in source,
            "phase_freeze_missing")
    require("AccumulatorOnlyDr7()" not in loop,
            "single_address_dr7_in_phase_loop")
    require("kWorldCommitCompletion" in source,
            "world_commit_completion_flag_missing")
    require("host_session::Cleanup(" in source,
            "mandatory_controller_cleanup_missing")
    require("final_writer::ConditionalRollback(" in source,
            "mandatory_writer_cleanup_missing")

    print(
        "PHASE_PACED_EXECUTOR_POLICY passed=1 hwbp_addresses=4 "
        "boundaries=delta_c98_c9c_world external_writes=fixed_delta_only "
        "game_owned_controls=1 game_owned_actions=1 game_owned_writer=1 "
        "final_in_flight_at_world_commit=1 mandatory_cleanup=1 device_access=0"
    )


if __name__ == "__main__":
    main()
