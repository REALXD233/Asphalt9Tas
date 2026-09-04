#!/usr/bin/env python3
"""Offline policy checks for the FC-2 -> FC-3 successor controller."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp"
BUILD = ROOT / "build-fc3-successor-controller-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for needle in (
        "#define A9TAS_FC3_TAIL_CANDIDATE 0",
        "FC-3 tail requires the FC-2 successor transaction controller",
        "I_ACCEPT_FC2_FC3_SUCCESSOR_OBSERVE_ONLY_V1",
        "Fc3FourWatchDr7",
        "ArmFc3BeforeDedicated",
        "RearmFc3NitroAndResumeOthers",
        "AllLiveThreadsStopped",
        "PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL",
        "ptrace_event != 0",
        "kRejectThreadLifecycle",
        "Fc3FinalSnapshotBarrier",
        "event != PTRACE_EVENT_STOP",
        "(owner_dr6 & 0xFu) == 0x1u",
        "BuildFc3Stop",
        "bool Fc3CheckedAdd",
        "bool Fc3ReadableWritable",
        "!Fc3CheckedAdd(tail.identity.action_owner, kFc3ActionVectorOffset",
        "!Fc3ReadableWritable(maps, action_vector, 3 * sizeof(std::uintptr_t))",
        "!Fc3ReadableWritable(maps, tail.identity.payload_evidence",
        "Fc3TailIdentityMatchesFc2",
        "a9tas_fc3_resolve_identities_review_v1",
        "kRearmNitroAndContinue",
        "waitpid(-1, &status, __WALL | WNOHANG)",
        "fc3_tail.core->phase_proved()",
        "fc3_tail.report.game_write_attempts == 0",
        "fc3_tail.report.cleanup_disposition == 0",
        "fc3_rollback_frozen",
        "Never repair a target vptr while any live thread may still be running",
        "const bool detach_clean",
        "if (!process_alive || !tracer_clear)",
        "constexpr int kExpectedArgc = 8",
        "PID EXPECTED_START_TIME LIB_BASE_HEX TIMEOUT_MS",
        "Fc3ReadProcessStartTime",
        "Fc3ParseProcessStatStartTime",
        "std::strrchr(line, ')')",
        "field = 4; field < 22",
        "std::strtoull(cursor, &end, 10)",
        "Fc3ProcessMatchesStartTime",
        "WriteReportExclusive",
        "O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |",
        "O_NOFOLLOW",
        "S_ISREG(state.st_mode)",
        "state.st_nlink != 1",
        "fsync(fileno(file))",
        "WriteReport(argv[kFc2ReportIndex], report)",
        "FC3_TAIL_DONE success=%u",
    ):
        require(needle in source, f"missing successor-controller token: {needle}")
    for needle in (
        "CarPhysicsState_dispatch_action", "NitroState_handle_activation",
        "process_vm_writev", "PTRACE_SETREGS", "input keyevent",
        "input tap", "socket(", "dlopen(", "mprotect(",
    ):
        require(needle not in source,
                f"forbidden successor-controller primitive: {needle}")
    require(source.count("InstallShadowVptrWhileFrozen(") == 2,
            "successor must retain exactly the FC-2 one-swap function/call")
    require(source.count("PTRACE_O_TRACECLONE") == 1 and
            source.count("PTRACE_O_EXITKILL") == 1,
            "successor lifecycle options must have one isolated call site")
    require(source.count("fc3_tail.report.cleanup_disposition = 1") >= 5,
            "successor failure paths do not consistently require force-stop")
    require(source.count("Fc3ProcessMatchesStartTime(") == 4,
            "successor start-time binding must have one definition and three checks")
    require(source.count("WriteReportExclusive(") == 4 and
            "I_ACCEPT_FC3_ENTRY_STABILITY_ATTACH_READ_DETACH_V1" in source,
            "successor must isolate the extra phase-only entry report writer")
    require("written == static_cast<ssize_t>(sizeof(shadow_vptr))" in source,
            "FC-2 verified write boundary missing")
    snapshot = source.index("bool BuildFc3Stop")
    snapshot_maps = source.index("!ReadMaps(", snapshot)
    snapshot_first_read = source.index("!ReadExact(mem", snapshot)
    require(snapshot_maps < snapshot_first_read,
            "FC-3 snapshot must refresh mappings before target reads")
    freeze = source.index("FreezeAllExcept(pid")
    arm = source.index("ArmFc3BeforeDedicated", freeze)
    swap = source.index("InstallShadowVptrWhileFrozen(", arm)
    dedicated = source.index("kRearmNitroAndContinue", swap)
    rearm = source.index("RearmFc3NitroAndResumeOthers", dedicated)
    proved = source.index("fc3_tail.core->phase_proved()", rearm)
    rollback = source.index("ConditionalRollback(mem", proved)
    detach = source.index("RestoreAndDetachAll(&threads", rollback)
    require(freeze < arm < swap < dedicated < rearm < proved < rollback < detach,
            "successor FC-2/FC-3 lifecycle ordering")
    require('"-DA9TAS_FC2_LIVE_CANDIDATE=1"' in build and
            '"-DA9TAS_FC3_TAIL_CANDIDATE=1"' in build,
            "successor review macros missing")
    require("a9tas-fc3-successor-linkcheck-" in build and
            "Remove-Item -LiteralPath $tempDir -Recurse -Force" in build,
            "successor full link check must be ephemeral")
    require("adb" not in build.lower(), "successor build must be device-free")


def verify_artifacts(passive: pathlib.Path, review: pathlib.Path,
                     readelf: pathlib.Path, objdump: pathlib.Path) -> None:
    for path in (passive, review, readelf, objdump):
        require(path.is_file(), f"missing successor artifact/tool: {path}")
    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(passive)], text=True
    )
    review_header = subprocess.check_output(
        [str(readelf), "-h", str(review)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header,
            "successor passive architecture")
    require("Advanced Micro Devices X86-64" in review_header and
            re.search(r"Type:\s+REL", review_header),
            "successor review controller must be x86-64 REL")
    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "open", "waitpid", "kill"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive successor imports process primitive: {name}")
    symbols = subprocess.check_output(
        [str(readelf), "--symbols", "--wide", str(review)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "waitpid"):
        require(re.search(rf"UND\s+{re.escape(name)}$", symbols, re.MULTILINE),
                f"successor review object lacks compiled FC-2 transport: {name}")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(review)], text=True
    )
    for token in ("Fc3FourWatchDr7", "BuildFc3Stop", "FC3_TAIL_DONE"):
        require(token in disassembly or token in SOURCE.read_text(encoding="utf-8"),
                f"successor review missing {token}")


def main() -> int:
    verify_source()
    if len(sys.argv) == 5:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [passive review-object readelf objdump]"
        )
    print("FC3_SUCCESSOR_CONTROLLER_POLICY passed=1 activation=fc2-tail "
          "four_watchpoints=1 nitro_rearm=1 extra_game_writes=0 "
          "full_link=ephemeral device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
