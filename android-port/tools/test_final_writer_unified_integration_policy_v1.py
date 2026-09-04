#!/usr/bin/env python3
"""Offline policy/ABI proof for the guarded final-writer unified candidate."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPOSER = ROOT / "src" / "hwbp_final_writer_unified_replay_v1.cpp"
EXECUTOR = ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp"
INTEGRATION = ROOT / "src" / "final_writer_unified_integration_v1.h"
BUILD = ROOT / "build-final-writer-unified-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    composer = COMPOSER.read_text(encoding="utf-8")
    executor = EXECUTOR.read_text(encoding="utf-8")
    integration = INTEGRATION.read_text(encoding="utf-8")
    required_composer = (
        "#define A9TAS_FINAL_WRITER_LIVE_CANDIDATE 0",
        "#if A9TAS_FINAL_WRITER_LIVE_CANDIDATE == 1",
        "A9TAS_FINAL_WRITER_REPLAY_V1 1",
        "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1",
        "g_a9tas_final_writer_target_blob_path_v1 = argv[6]",
        "g_a9tas_final_writer_payload_report_path_v1 = argv[8]",
        "ReadProcessStartTicks",
        "expected_start_ticks",
        "READY_NO_ATTACH_FINAL_WRITER_V1",
        "READY_ARMED_FINAL_WRITER_V1",
        "PrearmFinalWriterUntilResume",
        "paused baseline changed sample=%d",
        "delta_us=%\" PRId64",
        "payload_mapped=1",
        "host_resume_gate=marker_removal",
        "FINAL_WRITER_UNIFIED_BUILD_ONLY runtime=disabled return=-100",
    )
    required_executor = (
        "final_writer_unified_integration_v1.h",
        "'A', '9', 'U', 'E', 'R', '8'",
        "final_writer_begin_cursor_mismatch",
        "#ifndef A9TAS_FINAL_WRITER_REPLAY_V1\n"
        "                               && delta_us <= 1000000",
        "FinalWriterStartAnchorStage::kAwaitC98",
        "FinalWriterStartAnchorStage::kAwaitC9C",
        "FinalWriterStartAnchorStage::kAwaitDeltaZero",
        "FinalWriterStartAnchorStage::kReady",
        "final_writer_start_anchor_complete",
        "final_writer_start_anchor_input_order",
        "ArmWriterForTick",
        "final_writer_frame_permit_failed",
        "InstallBeforeResume",
        "ResumeAfterPrearm",
        "AttachNewThreadsStopped",
        "InstallAtCertifiedPrefix",
        "final_writer_owner_lost",
        "AcknowledgeAtCallbackClose",
        "CommitAtWorldBoundary",
        "ResumeFrozenOthers",
        "final_writer_final_ok",
        "WritePayloadReport",
        "final_writer_payload_report_ok",
        "ConditionalRollback",
        "thread_abnormal_exit",
        "WEXITSTATUS(status) != 0",
    )
    required_integration = (
        "final_writer_replay_elf_v1::Resolve",
        "final_writer_target_blob_v1::Decode",
        "final_writer_transaction_core_v1::Prepare",
        "final_writer_storage_transaction_v1::Stage",
        "final_writer_cursor_binding_v1::Binding",
        "FreezeOthers", "external_index != 0",
        "prearm_all_threads_frozen",
        "PrearmInstallResult", "kThreadSetDidNotStabilize",
        "current != runtime->prepared.unpublished_control.original_vptr",
        "AcknowledgeWriter", "CommitTick", "ValidateFinal",
        "FramePermit(external_index)",
        "offsetof(Control, reserved)",
        "std::memcmp(&live_control, &runtime->prepared.published_control",
        "current != control.shadow_vptr", "ResetUninstalled",
        "O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC",
        "header.original_vptr",
        "std::memset(&header, 0, sizeof(header))",
    )
    for needle in required_composer:
        require(needle in composer, f"composer policy missing: {needle}")
    for needle in required_executor:
        require(needle in executor, f"executor integration missing: {needle}")
    for needle in required_integration:
        require(needle in integration, f"integration policy missing: {needle}")

    begin = executor.index("final_writer_begin_cursor_mismatch")
    permit = executor.index("ArmWriterForTick", begin)
    start_anchor = executor.index("final_writer_start_anchor_complete")
    prefix = executor.index("InstallAtCertifiedPrefix")
    acknowledge = executor.index("AcknowledgeAtCallbackClose")
    commit = executor.index("CommitAtWorldBoundary")
    require(commit < acknowledge < begin < prefix,
            "textual layout sanity changed unexpectedly")
    require(begin < permit, "frame permit is not armed after cursor begin")
    require(start_anchor < begin < permit,
            "start anchor can consume or permit replay frame zero")
    anchor_block = executor[
        executor.index("MainTimeSource+0x150 is written"):
        executor.index("#endif", start_anchor)
    ]
    require("WriteExactVerified" not in anchor_block and
            "BeginTick" not in anchor_block and
            "ArmWriterForTick" not in anchor_block,
            "start anchor is not strictly observational")
    # Runtime order is encoded by event branches, not textual function order:
    # delta -> C9C certified prefix -> callback close -> world commit.
    require("Event::kDeltaNonzero" in executor[:begin], "begin is not in delta branch")
    require("kUnifiedPrefixCertified" in executor[:prefix],
            "install is not after prefix certification")
    require("dispatching == 0" in executor[:acknowledge],
            "acknowledgement is not in callback-close branch")
    require("machine.stage == Stage::kWaitWorldCommit" in executor[:commit],
            "commit binding is not at world commit")

    freeze_start = integration.index("inline bool FreezeOthers")
    freeze_end = integration.index("inline bool InstallAtCertifiedPrefix", freeze_start)
    freeze_body = integration[freeze_start:freeze_end]
    require("AttachNewThreadsStopped(" in freeze_body,
            "freeze/rollback closure may resume newly attached threads")
    require("AttachNewThreads(" not in freeze_body,
            "freeze/rollback closure retains a run-then-stop window")

    cleanup = executor.index("const bool final_writer_cleanup_frozen")
    rollback = executor.index("ConditionalRollback", cleanup)
    detach = executor.index("for (auto& thread : threads)", rollback)
    cleanup_body = executor[cleanup:detach]
    require("if (final_writer_cleanup_frozen)" in cleanup_body and
            "if (final_writer_final_ok)" in cleanup_body and rollback < detach,
            "failed cleanup freeze can bypass conditional vptr rollback")

    old_write = executor.index("backend.native_pose_address", acknowledge)
    guarded_else = executor.rfind("#else", acknowledge, old_write)
    require(guarded_else > acknowledge,
            "legacy callback-close correction is not isolated behind #else")
    for forbidden in ("dlopen(", "dlsym(", "process_vm_writev", "mprotect(",
                      "input keyevent", "input tap", "Nitro"):
        require(forbidden not in integration, f"forbidden integration primitive: {forbidden}")

    build = BUILD.read_text(encoding="utf-8")
    require('"-DA9TAS_FINAL_WRITER_LIVE_CANDIDATE=1" "-c"' in build,
            "live candidate must remain unlinked review object")
    require("a9tas-final-writer-unified-linkcheck-" in build and
            "Remove-Item -LiteralPath $linkCheck -Force" in build,
            "ephemeral link-check cleanup missing")
    require("adb" not in build.lower(), "build script accesses a device")


def verify_artifacts(passive: pathlib.Path, live_object: pathlib.Path,
                     readelf: pathlib.Path, objdump: pathlib.Path) -> None:
    for path in (passive, live_object, readelf, objdump):
        require(path.is_file(), f"missing artifact/tool: {path}")
    passive_header = subprocess.check_output([str(readelf), "-h", str(passive)], text=True)
    live_header = subprocess.check_output([str(readelf), "-h", str(live_object)], text=True)
    require("Advanced Micro Devices X86-64" in passive_header, "passive architecture")
    require(re.search(r"Type:\s+REL", live_header) is not None,
            "live artifact is not relocatable")
    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True)
    for name in ("ptrace", "pwrite", "pread", "waitpid", "kill"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive artifact imports {name}")
    live_symbols = subprocess.check_output(
        [str(readelf), "--symbols", "--wide", str(live_object)], text=True)
    for name in ("ptrace", "pwrite", "pread", "waitpid"):
        require(re.search(rf"UND\s+{name}$", live_symbols, re.MULTILINE) is not None,
                f"review object did not compile {name}")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(live_object)], text=True)
    require("a9tas_final_writer_unified_embedded_main_v1" in disassembly,
            "embedded integrated executor optimized away")


def main() -> int:
    verify_source()
    if len(sys.argv) == 5:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [passive live-object readelf objdump]")
    print("FINAL_WRITER_UNIFIED_POLICY passed=1 original_first=1 install_before_resume=1 "
          "fallback_install_at_c9c=1 "
          "ack_at_callback_close=1 commit_bound=1 legacy_close_write=0 "
          "live_object=unlinked device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
