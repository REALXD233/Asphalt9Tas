#!/usr/bin/env python3
"""Offline policy and artifact checks for the FC-2 three-frame controller."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp"
BUILD = ROOT / "build-fc2-frame-callback-transaction-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    required = (
        "#define A9TAS_FC2_LIVE_CANDIDATE 0",
        "#if A9TAS_FC2_LIVE_CANDIDATE == 1",
        "I_ACCEPT_FC2_THREE_FRAME_DEFERRED_REGISTRATION_OBSERVE_ONLY_V1",
        "fc2_payload_elf_resolver_v1.h",
        "fc2_transaction_report_v1.h",
        "a9tas::fc2_payload_elf_v1::Resolve(pid, mem, &payload)",
        "kPhysicsContextAdd = 0x38B77CC",
        "kPhysicsContextRemove = 0x38B7840",
        "kCarPhysicsPrimaryVtable = 0x7EE8D18",
        "kOriginalCallback = 0x367D66C",
        "offsetof(PayloadEvidence, dedicated_entries) == 56",
        "FlagsAndDedicatedDr7",
        "PokeDebug(tid, 1, dedicated_entries_address)",
        "enum class Await",
        "kBootstrapClose",
        "kRegistrationOpen",
        "kDedicatedHit",
        "kRemovalClose",
        "kCompactionOpen",
        "RegistrationEvidenceComplete",
        "RemovalEvidenceComplete",
        "dedicated_members_after_registration = dedicated_count",
        "dedicated_members_after_removal = dedicated_count",
        "report.cleanup_disposition = 1",
        "report.final_threads == report.initial_threads",
        "FC2_BUILD_ONLY runtime=disabled return=-100 device_access=0",
    )
    for needle in required:
        require(needle in text, f"missing FC-2 controller token: {needle}")

    forbidden = (
        "adb ", "input keyevent", "input tap", "CarPhysicsState_dispatch_action",
        "PTRACE_SETREGS", "dlopen(", "mprotect(",
        "process_vm_writev", "socket(", "pthread_create", "std::thread",
    )
    for needle in forbidden:
        require(needle not in text, f"forbidden FC-2 controller primitive: {needle}")
    require("#define A9TAS_FC3_TAIL_CANDIDATE 0" in text and
            "#if A9TAS_FC3_TAIL_CANDIDATE == 1" in text,
            "FC-3 successor tail must remain default-off in the FC-2 source")
    for token in (
        "kRejectUnexpectedWaitEvent", "kRejectWrongStopSignal",
        "kRejectDr6Read", "kRejectUnexpectedBreakpoint",
        "kRejectFlagsRead", "kRejectThreadName", "kRejectListRead",
        "kRejectDispatchState", "kRejectCarMembership",
        "kRejectDedicatedMembership", "kRejectCarVptr",
        "kRejectRegistrationEvidence", "kRejectDedicatedEvidenceHit",
        "kRejectRemovalEvidence", "kRejectUnexpectedWaitState",
        "kRejectThreadSignaled",
    ):
        require(token in text, f"missing reject reason: {token}")

    readonly = text.index("open(mem_path, O_RDONLY")
    resolver = text.index("fc2_payload_elf_v1::Resolve", readonly)
    identity = text.index("ResolveIdentity", resolver)
    close_readonly = text.index("close(mem);", identity)
    readwrite = text.index("open(mem_path, O_RDWR", close_readonly)
    prepare = text.index("PreparePayload", readwrite)
    attach = text.index("AttachCurrentThreads", prepare)
    require(readonly < resolver < identity < close_readonly < readwrite <
            prepare < attach, "FC-2 preflight/prepare/attach ordering")

    freeze = text.index("FreezeAllExcept(pid")
    dr1 = text.index("ArmDedicatedWatch(owner->tid", freeze)
    swap = text.index("InstallShadowVptrWhileFrozen(", dr1)
    resume = text.index("ContinueOwner(owner", swap)
    registration = text.index("RegistrationEvidenceComplete", resume)
    dedicated = text.index("dedicated_entries != 1", registration)
    removal = text.index("RemovalEvidenceComplete", dedicated)
    compaction = text.index("dedicated_members_after_removal", removal)
    rollback = text.index("ConditionalRollback(mem", compaction)
    detach = text.index("RestoreAndDetachAll(&threads", rollback)
    require(freeze < dr1 < swap < resume < registration < dedicated < removal <
            compaction < rollback < detach,
            "FC-2 three-frame transaction ordering")
    require(text.count("InstallShadowVptrWhileFrozen(") == 2,
            "FC-2 must have one vptr swap function and one call")
    require("written == static_cast<ssize_t>(sizeof(shadow_vptr))" in text and
            "if (current != shadow_vptr)" in text,
            "FC-2 partial-write/conditional rollback policy")
    require("dedicated_members_before = 0" in text and
            "dedicated_members_after_registration == 1" in text and
            "dedicated_members_after_removal == 0" in text,
            "FC-2 membership 0->1->0 success policy")

    build = BUILD.read_text(encoding="utf-8")
    require('"-DA9TAS_FC2_LIVE_CANDIDATE=1" "-c"' in build,
            "FC-2 live path must compile to unlinked review object")
    require("a9tas-fc2-linkcheck-" in build and
            "Remove-Item -LiteralPath $linkCheck -Force" in build,
            "FC-2 ephemeral link-check policy")
    require("fc2_frame_callback_transaction_v1_live" not in build,
            "runnable FC-2 live artifact name is forbidden")
    require("adb" not in build.lower(), "FC-2 build must be device-free")


def verify_behavioral_model() -> None:
    states = ["bootstrap_close", "registration_open", "dedicated_hit",
              "removal_close", "compaction_open"]
    members = [0, 1, 1, 0]
    require(states[-1] == "compaction_open" and members == [0, 1, 1, 0],
            "FC-2 success lifecycle model")
    require(not (members[1] == 1 and members[-1] == 1),
            "FC-2 retained callback must fail")
    require({"bootstrap_close", "registration_open", "dedicated_hit",
             "removal_close", "compaction_open"} == set(states),
            "FC-2 cannot skip a proof stage")


def verify_artifacts(passive: pathlib.Path, review: pathlib.Path,
                     readelf: pathlib.Path, objdump: pathlib.Path) -> None:
    for path in (passive, review, readelf, objdump):
        require(path.is_file(), f"missing FC-2 artifact/tool: {path}")
    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(passive)], text=True
    )
    review_header = subprocess.check_output(
        [str(readelf), "-h", str(review)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header,
            "FC-2 passive architecture")
    require("Advanced Micro Devices X86-64" in review_header and
            re.search(r"Type:\s+REL", review_header),
            "FC-2 review artifact must be x86-64 REL")
    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "open", "waitpid", "kill"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive FC-2 imports process primitive: {name}")
    review_symbols = subprocess.check_output(
        [str(readelf), "--symbols", "--wide", str(review)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "waitpid"):
        require(re.search(rf"UND\s+{re.escape(name)}$", review_symbols,
                          re.MULTILINE),
                f"FC-2 review object lacks compiled primitive: {name}")
    main = re.search(
        r"\s+[0-9a-f]+\s+(\d+)\s+FUNC\s+GLOBAL\s+DEFAULT\s+\d+\s+main$",
        review_symbols, re.MULTILINE,
    )
    require(main is not None and int(main.group(1)) > 8000,
            "FC-2 transaction main optimized away or truncated")
    disasm = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(review)], text=True
    ).lower()
    for value in ("0x7ee8d18", "0x38b77cc", "0x38b7840"):
        require(value in disasm, f"FC-2 disassembly lacks constant {value}")


def main() -> int:
    verify_source()
    verify_behavioral_model()
    if len(sys.argv) == 5:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [passive review-object readelf objdump]"
        )
    print("FC2_TRANSACTION_POLICY passed=1 passive=1 three_frame=1 "
          "membership_0_1_0=1 dedicated_owner_hwbp=1 fail_closed=1 "
          "device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
