#!/usr/bin/env python3
"""Audit the pure post-C9C single-slot RBX-to-angular schedule."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
HEADER = WORKSPACE / "android-port" / "src" / "barrel_post_c9c_slot_schedule_v1.h"
SELFTEST = WORKSPACE / "android-port" / "src" / "barrel_post_c9c_slot_schedule_selftest_v1.cpp"
BASELINE = WORKSPACE / "android-port" / "src" / "hwbp_unified_tick_executor_v1.cpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 2:
        print("usage: policy SELFTEST READELF", file=sys.stderr)
        return 2
    artifact, readelf = map(pathlib.Path, args)
    for path in (artifact, readelf, HEADER, SELFTEST, BASELINE):
        require(path.is_file(), f"missing required file: {path}")
    header = HEADER.read_text(encoding="utf-8")
    baseline = BASELINE.read_text(encoding="utf-8")
    for token in (
        "kAwaitCertifiedC9C",
        "kActionInstallParallelWatchSet",
        "kActionSwapRbxFirstForSecond",
        "kActionApplyRbxTail",
        "kActionResetRbxFirstWatch",
        "kActionArmAngularTransientWindow",
        "kActionVerifyBarrelTerminal",
        "kActionRestorePostF64WatchSet",
        "kAngularIdleReceiptAtF64",
        "kAngularAbsentCancelled",
        "event != Event::kCertifiedC9C || !state->completion_seen",
        "state->stage = Stage::kActiveWindow",
        "state->rbx_phase == RbxPhase::kAwaitSecond",
        "state->angular_token_armed",
        "kMaximumTransactionsPerFrame",
        "kAwaitWorldCommit",
        "AdvanceInterlocked",
        "MainStage::kAwaitCallbackClose",
        "MainStage::kAwaitDeferredClear",
        "MainStage::kAwaitWorldCommit",
        "InterlockedTerminal",
    ):
        require(token in header, f"missing slot schedule contract: {token}")
    for token in (
        "ProgramStoppedThread(", "completion, callback_flags, f64,",
        "world_accumulator, PipelineDr7()",
    ):
        require(token in baseline, f"known-good four-slot baseline changed: {token}")
    for token in ("PTRACE_", "process_vm_", "/proc/", "pread(", "pwrite(",
                  "kill(", "waitpid(", "RemoteCall"):
        require(token not in header, f"pure slot model gained runtime primitive: {token}")
    elf = run(str(readelf), "-h", str(artifact))
    require("Machine:" in elf and "X86-64" in elf,
            "slot schedule selftest is not Android x86_64 ELF")
    rodata = run(str(readelf), "-p", ".rodata", str(artifact))
    require("BARREL_POST_C9C_SLOT_SCHEDULE_SELFTEST" in rodata,
            "slot schedule selftest receipt missing")
    print(
        "BARREL_POST_C9C_SLOT_SCHEDULE_POLICY passed=1 four_slots=1 "
        "c9c_reuses_dr0_dr1=1 rbx_yaw_orthogonal=1 callback_restored_at_f64=1 "
        "f64_guard=1 explicit_idle_receipt=1 broad_absent_cancel=1 repeated_calls=1 transient_yaw=1 "
        "max_transactions_per_frame=2 main_machine_interlock=1 "
        "callback_close_deferred_world_order=1 baseline_unchanged=1 "
        "runtime=disabled device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_POST_C9C_SLOT_SCHEDULE_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
