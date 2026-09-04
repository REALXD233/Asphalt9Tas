#!/usr/bin/env python3
"""Cross-artifact closure audit for the build-only FC-3 successor candidate."""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTROLLER_SOURCE = ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp"
IDENTITY_SOURCE = ROOT / "src" / "fc3_replay_observer_identity_resolver_v1.cpp"
EVENT_SOURCE = ROOT / "src" / "fc3_observer_event_core_v1.cpp"
PROTOCOL_SOURCE = ROOT / "src" / "fc3_replay_observer_state_machine_v1.cpp"
VALIDATOR = ROOT / "tools" / "validate_fc3_report_v1.py"
BUILD = ROOT / "build-fc3-successor-controller-v1.ps1"
SUCCESSOR_DIR = ROOT / "build" / "fc3-successor-controller-v1"

EXPECTED_HASHES = {
    "successor_passive": "84a84dd53e02d61c28477065c18a7c1fab1336fa14871c2ebf7572249adfe8a5",
    "successor_review": "9b9ed7a4af74521145f5b608fda710f538340fce07a1a22016aff7d97619ff46",
    "identity_review": "63a0882baa51a88d860114edd04ed8a9c1a20fa614d2466371fee729b5479f40",
    "event_review": "3cbbbe6fadeeebd9c2b66a31ac2d94d8540b0c42c6157fd5efeadefe540bdf1e",
    "protocol_review": "ea6dc99eb1fa3ca87a4955f9991438f1071103f91764db37b813ab4c8b158111",
    "fc2_review": "a797999eb8dc1655f757a25b6ac50f202e97a55a423cb96a6ed60f81809fba88",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbols(tool: pathlib.Path, artifact: pathlib.Path, dynamic: bool) -> str:
    option = "--dyn-syms" if dynamic else "--symbols"
    return subprocess.check_output(
        [str(tool), option, "--wide", str(artifact)], text=True
    )


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit(
            f"usage: {sys.argv[0]} SUCCESSOR_PASSIVE SUCCESSOR_REVIEW "
            "IDENTITY_REVIEW EVENT_REVIEW PROTOCOL_REVIEW FC2_REVIEW "
            "READELF OBJDUMP"
        )
    (successor_passive, successor_review, identity_review, event_review,
     protocol_review, fc2_review, readelf, objdump) = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    inputs = {
        "successor_passive": successor_passive,
        "successor_review": successor_review,
        "identity_review": identity_review,
        "event_review": event_review,
        "protocol_review": protocol_review,
        "fc2_review": fc2_review,
    }
    for path in (*inputs.values(), readelf, objdump, CONTROLLER_SOURCE,
                 IDENTITY_SOURCE, EVENT_SOURCE, PROTOCOL_SOURCE, VALIDATOR,
                 BUILD):
        require(path.is_file(), f"missing FC-3 closure input: {path}")
    actual = {name: sha256(path) for name, path in inputs.items()}
    require(actual == EXPECTED_HASHES, "FC-3 closure hash mismatch")

    expected_files = {
        "fc3_successor_controller_v1_build_only",
        "fc3_successor_controller_v1_review_only.o",
        "fc3_successor_controller_v1_review_only.disasm.txt",
    }
    require({path.name for path in SUCCESSOR_DIR.iterdir() if path.is_file()} ==
            expected_files, "linked/runnable successor artifact retained")

    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(successor_passive)], text=True
    )
    review_header = subprocess.check_output(
        [str(readelf), "-h", str(successor_review)], text=True
    )
    identity_header = subprocess.check_output(
        [str(readelf), "-h", str(identity_review)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header and
            re.search(r"Type:\s+DYN", passive_header), "passive ELF")
    require("Advanced Micro Devices X86-64" in review_header and
            re.search(r"Type:\s+REL", review_header), "successor review ELF")
    require("Advanced Micro Devices X86-64" in identity_header and
            re.search(r"Type:\s+DYN", identity_header), "identity review ELF")

    passive_dyn = symbols(readelf, successor_passive, True)
    for name in ("ptrace", "pwrite", "pread", "waitpid", "kill", "open"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_dyn) is None,
                f"passive successor imports {name}")
    require(b"FC2_BUILD_ONLY runtime=disabled return=-100" in
            successor_passive.read_bytes(), "passive inert marker")
    require(b"I_ACCEPT_FC2_FC3_SUCCESSOR_OBSERVE_ONLY_V1" not in
            successor_passive.read_bytes(), "passive contains live acknowledgement")

    review_symbols = symbols(readelf, successor_review, False)
    for name in ("ptrace", "pwrite", "pread", "waitpid"):
        require(re.search(rf"UND\s+{re.escape(name)}$", review_symbols,
                          re.MULTILINE),
                f"successor review transport missing: {name}")
    require("a9tas_fc3_resolve_identities_review_v1" in review_symbols,
            "successor lacks identity-resolver edge")
    review_table = subprocess.check_output(
        [str(objdump), "-t", "--demangle", str(successor_review)], text=True
    )
    require("a9tas::fc3_observer_event_core_v1::Core::Consume" in review_table,
            "successor lacks event-core edge")
    review_bytes = successor_review.read_bytes()
    for token in (b"I_ACCEPT_FC2_FC3_SUCCESSOR_OBSERVE_ONLY_V1",
                  b"FC3_TAIL_DONE success=%u"):
        require(token in review_bytes, f"successor protocol token missing: {token!r}")

    identity_dyn = symbols(readelf, identity_review, True)
    for name in ("ptrace", "pwrite", "process_vm_writev", "waitpid", "kill",
                 "socket", "dlopen", "mprotect"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", identity_dyn) is None,
                f"identity resolver imports forbidden primitive: {name}")
    require("a9tas_fc3_resolve_identities_review_v1" in identity_dyn,
            "identity resolver export missing")

    for artifact, label in ((event_review, "event"),
                            (protocol_review, "protocol")):
        artifact_symbols = symbols(readelf, artifact, False)
        for name in ("ptrace", "pwrite", "pread", "waitpid", "open"):
            require(re.search(rf"UND\s+{re.escape(name)}$", artifact_symbols,
                              re.MULTILINE) is None,
                    f"{label} review imports process primitive: {name}")

    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(successor_review)], text=True
    ).lower()
    for immediate in ("0xd9950055", "0x100008", "0x1368", "0x1378"):
        require(immediate in disassembly,
                f"successor disassembly lacks pinned constant {immediate}")
    relocations = subprocess.check_output(
        [str(objdump), "-dr", "--demangle", str(successor_review)], text=True
    )
    pwrite_relocations = list(re.finditer(
        r"R_X86_64_PLT32\s+pwrite(?:-|\s|$)", relocations))
    require(len(pwrite_relocations) == 2,
            "compiled successor must contain exactly two FC-2 pwrite edges")
    function_headers = list(re.finditer(
        r"^[0-9a-f]+ <(.+)>:$", relocations, re.MULTILINE))
    pwrite_owners = []
    for relocation in pwrite_relocations:
        preceding = [header for header in function_headers
                     if header.start() < relocation.start()]
        require(preceding, "pwrite relocation has no compiled function owner")
        pwrite_owners.append(preceding[-1].group(1))
    require(sum("InstallShadowVptrWhileFrozen" in owner
                for owner in pwrite_owners) == 1 and
            sum("WriteExactVerified" in owner
                for owner in pwrite_owners) == 1,
            "compiled pwrite owners are not the vptr installer and verified-write helper")

    waitpid_relocations = list(re.finditer(
        r"R_X86_64_PLT32\s+waitpid(?:-|\s|$)", relocations))
    waitpid_owners = []
    for relocation in waitpid_relocations:
        preceding = [header for header in function_headers
                     if header.start() < relocation.start()]
        require(preceding, "waitpid relocation has no compiled function owner")
        owner_header = preceding[-1]
        owner = owner_header.group(1)
        waitpid_owners.append(owner)
        if owner == "main" or "Fc3InterruptAndWaitUntil" in owner:
            call_prefix = relocations[owner_header.end():relocation.start()]
            require("$0x40000001" in call_prefix[-256:],
                    f"active successor waitpid is not __WALL|WNOHANG: {owner}")
    require(waitpid_owners.count("main") == 2 and
            sum("Fc3InterruptAndWaitUntil" in owner
                for owner in waitpid_owners) == 1,
            "compiled successor bounded wait ownership changed")

    controller = CONTROLLER_SOURCE.read_text(encoding="utf-8")
    for forbidden in ("CarPhysicsState_dispatch_action",
                      "NitroState_handle_activation", "process_vm_writev",
                      "PTRACE_SETREGS", "input keyevent", "input tap"):
        require(forbidden not in controller,
                f"successor source contains forbidden call path: {forbidden}")
    require(controller.count("pwrite(") == 2,
            "FC-2 source pwrite surface changed")
    require(controller.count("WriteExactVerified(") == 6,
            "verified-write definition/call surface changed")
    compact_controller = re.sub(r"\s+", " ", controller)
    require(compact_controller.count(
                "WriteExactVerified(mem, car, &original_vptr, "
                "sizeof(original_vptr))") == 2,
            "conditional original-vptr restore call surface changed")
    for payload_call in (
            "WriteExactVerified(mem, payload.shadow, table.data(), table.size())",
            "WriteExactVerified(mem, payload.evidence, &expected_evidence, "
            "sizeof(expected_evidence))",
            "WriteExactVerified(mem, payload.control, &control, sizeof(control))"):
        require(compact_controller.count(payload_call) == 1,
                f"payload-only verified-write call changed: {payload_call}")
    require(controller.count("InstallShadowVptrWhileFrozen(") == 2,
            "FC-2 one-vptr function/call surface changed")
    for snapshot_guard in (
            "bool Fc3CheckedAdd",
            "bool Fc3ReadableWritable",
            "!Fc3CheckedAdd(tail.identity.action_owner, kFc3ActionVectorOffset",
            "!Fc3ReadableWritable(maps, action_vector, 3 * sizeof(std::uintptr_t))",
            "!Fc3ReadableWritable(maps, tail.identity.payload_evidence"):
        require(snapshot_guard in controller,
                f"successor snapshot address guard missing: {snapshot_guard}")
    snapshot = controller.index("bool BuildFc3Stop")
    snapshot_maps = controller.index("!ReadMaps(", snapshot)
    snapshot_first_read = controller.index("!ReadExact(mem", snapshot)
    require(snapshot_maps < snapshot_first_read,
            "successor snapshot reads precede refreshed mapping validation")
    require("fc3_rollback_frozen" in controller and
            "AllLiveThreadsStopped(threads)" in controller and
            "rollback_vptr == report.shadow_vptr && fc3_rollback_frozen" in
            controller,
            "successor rollback is not gated by a complete freeze")
    require("PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL" in controller and
            "preowner_ptrace_event != 0" in controller and
            "ptrace_event != 0" in controller and
            controller.count("kRejectThreadLifecycle") >= 2,
            "successor does not fail closed on pre-owner and transaction lifecycle events")
    for identity_commit_guard in (
            "constexpr int kExpectedArgc = 8",
            "PID EXPECTED_START_TIME LIB_BASE_HEX TIMEOUT_MS",
            "Fc3ParseProcessStatStartTime",
            "std::strrchr(line, ')')",
            "field = 4; field < 22",
            "std::strtoull(cursor, &end, 10)",
            "Fc3ReadProcessStartTime",
            "O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |",
            "O_NOFOLLOW",
            "S_ISREG(state.st_mode)",
            "state.st_nlink != 1",
            "fsync(fileno(file))",
            "WriteReport(argv[kFc2ReportIndex], report)"):
        require(identity_commit_guard in controller,
                f"successor identity/report commit guard missing: {identity_commit_guard}")
    require(controller.count("Fc3ProcessMatchesStartTime(") == 4 and
            controller.count("WriteReportExclusive(") == 4 and
            "I_ACCEPT_FC3_ENTRY_STABILITY_ATTACH_READ_DETACH_V1" in controller,
            "successor start-time/report commit call surface changed")
    for bounded_wait_guard in (
            "while (MonotonicNs() < deadline_ns)",
            "waitpid(tid, &status, __WALL | WNOHANG)",
            "Fc3InterruptAndWaitUntil",
            "Fc3StopThreadUntil",
            "WSTOPSIG(status) != SIGTRAP",
            ") != PTRACE_EVENT_STOP",
            "(dr6 & 0xFu) == 0",
            "stop_deadline_ns = MonotonicNs() + kFc3StopWaitNs",
            "A9TAS_STOP_THREAD_UNTIL(thread.tid, stop_deadline)"):
        require(bounded_wait_guard in controller,
                f"successor bounded stop wait missing: {bounded_wait_guard}")
    require("waitpid(tid, &status, __WALL)" not in controller and
            controller.count(
                "A9TAS_STOP_THREAD_UNTIL(thread.tid, stop_deadline)") == 3 and
            controller.count(
                "const std::uint64_t stop_deadline = "
                "MonotonicNs() + kFc3StopWaitNs") >= 2 and
            "AttachCurrentThreads(pid, flags_address, false, threads" in
            controller and
            ", stop_deadline" in controller[
                controller.index(
                    "AttachCurrentThreads(pid, flags_address, false, threads"):
                controller.index(
                    "AttachCurrentThreads(pid, flags_address, false, threads") + 256] and
            "Fc3FinalSnapshotBarrier(pid, transaction_tid," in controller and
            "transaction_deadline, &threads" in controller,
            "successor stop waits do not share bounded deadlines")
    freeze = controller.index("const bool initial_frozen = FreezeAllExcept(")
    owner_reacquire = controller.index(
        "owner = FindWatched(&threads, transaction_tid)", freeze)
    owner_resume = controller.index("ContinueOwner(owner, &report)",
                                    owner_reacquire)
    rollback_reacquire = controller.index(
        "owner = FindWatched(&threads, transaction_tid)", owner_resume)
    rollback_tid = controller.index(
        "const pid_t rollback_owner_tid = transaction_tid",
        rollback_reacquire)
    require(freeze < owner_reacquire < owner_resume < rollback_reacquire <
            rollback_tid,
            "owner pointer is not reacquired after possible vector growth")
    require("Fc3FinalSnapshotBarrier" in controller and
            "event != PTRACE_EVENT_STOP" in controller and
            "(owner_dr6 & 0xFu) == 0x1u" in controller and
            "final_snapshot_candidate" in controller,
            "successor final snapshot lacks a stopped-thread DR6 barrier")
    require("fc3_tail.report.cleanup_disposition == 0" in controller,
            "controller success omits FC-3 cleanup disposition")
    require("const bool detach_clean" in controller and
            "if (!process_alive || !tracer_clear)" in controller and
            controller.count("fc3_tail.report.cleanup_disposition = 1") >= 5,
            "post-rearm cleanup/survival failures do not select force-stop")

    identity_source = IDENTITY_SOURCE.read_text(encoding="utf-8")
    for identity_guard in (
            "bool CheckedAdd",
            "!CheckedAdd(owner, kFc3ActionVectorOffset, &vector)",
            "!CheckedAdd(physics_owner, kFc3NitroServiceOffset",
            "!CheckedAdd(context, kFc3CallbackListOffset, &callback_list)"):
        require(identity_guard in identity_source,
                f"identity derived-address guard missing: {identity_guard}")
    prepare = controller.index("PreparePayload(mem")
    attach = controller.index("AttachCurrentThreads", prepare)
    swap = controller.index("InstallShadowVptrWhileFrozen(", attach)
    tail = controller.index("fc3_tail.core->Consume", swap)
    rollback = controller.index("ConditionalRollback(mem", tail)
    require(prepare < attach < swap < tail < rollback,
            "payload/attach/swap/tail/rollback order")

    validator = VALIDATOR.read_text(encoding="utf-8")
    for needle in ("REPORT_SIZE = 512", "REQUIRED_FLAGS = 0x3FFFF",
                   "cross_validate_fc2", "FC-2/FC-3 identity or thread mismatch",
                   "FC-2 one-write transaction missing",
                   "fc2_validator.validate(fc2_data)",
                   "FC-2/FC-3 transaction timeline mismatch",
                   "timeline_adjacencies=7",
                   "cross-validator accepted non-strict timeline edge"):
        require(needle in validator, f"validator closure missing: {needle}")
    build = BUILD.read_text(encoding="utf-8")
    require("a9tas-fc3-successor-linkcheck-" in build and
            "Remove-Item -LiteralPath $tempDir -Recurse -Force" in build,
            "ephemeral link cleanup contract")
    for legacy_symbol in ("A9TasSchedulerObserverMain_NotUsed",
                          "AttachNewThreads", "ClearAndDetach"):
        require(legacy_symbol in build,
                f"linked-image legacy path rejection missing: {legacy_symbol}")
    require("link retained unreachable legacy blocking path" in build,
            "linked-image dead blocking path audit missing")

    print("FC3_SUCCESSOR_CLOSURE_AUDIT passed=1 hashes=6 "
          "fc2_review_unchanged=1 identity=read_only event=offline "
          "dr7=0xd9950055 clone_options=0x100008 "
          "linked_live_artifact=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
