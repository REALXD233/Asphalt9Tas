#!/usr/bin/env python3
"""Static/artifact policy for the passive natural action callback consumer."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
PAYLOAD_SOURCE = ROOT / "src" / "payload_natural_action_callback_consumer_v1.cpp"
HOST_GATE = ROOT / "src" / "natural_action_host_gate_v1.h"
PHASE_GATE = ROOT / "src" / "authoritative_natural_action_phase_gate_v1.h"
BUILD = ROOT / "build-natural-action-callback-consumer-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    payload = PAYLOAD_SOURCE.read_text(encoding="utf-8")
    host = HOST_GATE.read_text(encoding="utf-8")
    phase = PHASE_GATE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    for token in (
        "constexpr bool kActionExecutionCompiled = false",
        "static_assert(!kActionExecutionCompiled",
        "a9tas_natural_action_callback_consumer_v1",
        "a9tas_natural_action_callback_arm_v1",
        "return kRejectedBuildOnly",
        "command.nitro_activations != 0",
        "CompleteNaturalCallback(",
        "a9tas_natural_action_callback_mailbox_data_v1",
        "a9tas_natural_action_callback_object_data_v1",
        "a9tas_natural_action_callback_vtable_data_v1",
        "static_assert(sizeof(Evidence) == 128",
    ):
        require(token in payload, f"missing passive-consumer token: {token}")
    for forbidden in (
        "0x367B414",
        "NitroService",
        "dispatch_action",
        "ptrace(",
        "pwrite(",
        "mprotect(",
        "socket(",
        "pthread_create",
        "dlopen(",
    ):
        require(forbidden not in payload,
                f"forbidden passive-consumer primitive: {forbidden}")

    nonzero = payload.index("if (command.nitro_activations != 0)")
    semantic_failure = payload.index("command.sequence, 0, false", nonzero)
    zero_success = payload.index("command.sequence, 0, true", semantic_failure)
    require(nonzero < semantic_failure < zero_success,
            "nonzero commands must fail before zero-call completion")

    for token in (
        "kSkipNitroActivation",
        "nitro_enabled ? frame.nitro_activation_count : 0",
        "kPreviousFramePending",
        "CancelBeforePublication",
        "MarkPublished",
        "CompletionMatches",
        "++state->next_sequence",
        "++state->next_frame",
    ):
        require(token in host, f"missing host-gate token: {token}")
    for forbidden in ("ptrace", "pwrite", "/proc/", "socket(", "adb"):
        require(forbidden not in host.lower(),
                f"host gate contains process primitive: {forbidden}")

    select = phase.index("SelectAtDelta")
    rollback = phase.index("RollbackAtDeltaZeroBeforeC98", select)
    publish = phase.index("MarkPublishedAtC98", rollback)
    receipt = phase.index("RequireReceiptAtCallbackClose", publish)
    commit = phase.index("CommitAtWorld", receipt)
    require(select < rollback < publish < receipt < commit,
            "authoritative action phases are not explicitly ordered")
    require("kReceiptMissing" in phase and "kPoisoned" in phase,
            "missing receipt must poison before world commit")
    for forbidden in ("ptrace", "pwrite", "/proc/", "socket(", "adb"):
        require(forbidden not in phase.lower(),
                f"phase gate contains process primitive: {forbidden}")

    require("aarch64-linux-android24-clang++.cmd" in build,
            "payload build must target Android ARM64")
    require("x86_64-linux-android24-clang++.cmd" in build,
            "host gate selftest must target Android x86_64")
    require('"-Wall", "-Wextra", "-Werror"' in build,
            "build must treat warnings as errors")
    require("adb" not in build.lower() and "device_access=0" in build,
            "build must remain device-free")


def verify_artifact(payload: pathlib.Path, readelf: pathlib.Path,
                    objdump: pathlib.Path) -> None:
    for path in (payload, readelf, objdump):
        require(path.is_file(), f"missing artifact/tool: {path}")
    header = subprocess.check_output([str(readelf), "-h", str(payload)], text=True)
    require("AArch64" in header and re.search(r"Type:\s+DYN", header),
            "passive consumer is not AArch64 DSO")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(payload)], text=True
    )
    for name in (
        "a9tas_natural_action_callback_consumer_v1",
        "a9tas_natural_action_callback_arm_v1",
        "a9tas_natural_action_callback_protocol_v1",
        "a9tas_natural_action_callback_mailbox_data_v1",
        "a9tas_natural_action_callback_evidence_data_v1",
        "a9tas_natural_action_callback_object_data_v1",
        "a9tas_natural_action_callback_vtable_data_v1",
    ):
        require(re.search(rf"\b{re.escape(name)}$", symbols, re.MULTILINE),
                f"missing dynamic export: {name}")
    for name in ("ptrace", "pwrite", "mprotect", "socket", "pthread_create", "dlopen"):
        require(re.search(rf"UND\s+{name}(?:@|$)", symbols, re.MULTILINE) is None,
                f"passive consumer imports forbidden primitive: {name}")

    disasm = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(payload)], text=True
    )
    arm = re.search(
        r"<a9tas_natural_action_callback_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL,
    )
    arm_text = arm.group(1).lower() if arm else ""
    require(arm is not None and
            ("mov\tw0, #-100" in arm_text or
             ("mov\tw0, #-0x64" in arm_text and "=-100" in arm_text)),
            "arm export is not constant -100")


def main() -> int:
    verify_source()
    if len(sys.argv) == 4:
        verify_artifact(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [payload readelf objdump]")
    print("NATURAL_ACTION_CALLBACK_CONSUMER_POLICY passed=1 execute_compiled=0 "
          "arm_rejected=1 host_gate_pure=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
