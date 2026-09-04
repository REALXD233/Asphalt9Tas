#!/usr/bin/env python3
"""Cross-artifact audit for the local-only natural scheduler action payload."""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


EXPECTED_PASSIVE_SHA256 = (
    "60a726174ac2a613d6098db77f9e82bbf819923ffd4e798d1f66eda9db454a41"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def output(*args: str) -> str:
    return subprocess.check_output(args, text=True, errors="replace")


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: audit... ACTION_PAYLOAD PASSIVE_PAYLOAD READELF OBJDUMP SOURCE BUILD"
        )
    action, passive, readelf, objdump, source, build = map(
        pathlib.Path, sys.argv[1:]
    )
    for path in (action, passive, readelf, objdump, source, build):
        require(path.is_file(), f"missing audit input: {path}")

    passive_hash = hashlib.sha256(passive.read_bytes()).hexdigest()
    action_hash = hashlib.sha256(action.read_bytes()).hexdigest()
    require(passive_hash == EXPECTED_PASSIVE_SHA256,
            f"passive lifecycle payload drifted: {passive_hash}")
    require(action_hash != passive_hash,
            "action review payload must differ from passive payload")

    for payload in (action, passive):
        header = output(str(readelf), "-h", str(payload))
        require("AArch64" in header and re.search(r"Type:\s+DYN", header),
                f"not an AArch64 DSO: {payload}")

    source_text = source.read_text(encoding="utf-8")
    build_text = build.read_text(encoding="utf-8")
    for token in (
        "#define A9TAS_NAL_ACTION_EXECUTE 0",
        "kGameActionDispatchRva = 0x367B414",
        "ActionOwnerValid", "kActionDispatchVtableRva = 0x7EEFE68",
        "kActionDispatchVfunc158Rva = 0x36A9CAC",
        "ReadNitroState", "kNitroTransitionProof",
        "g_pending_action_sequence", "initial_active != 0",
        "kCommandQueueOffset = 0x1360",
        "kDirectModeOffset = 0x1378",
        "SubmitGameOwnedActions",
        "return common && control.vehicle_owner == control.expected_car",
        "control.vehicle_owner != control.expected_car",
        "after.count != previous.count + 1u",
        "dispatch(reinterpret_cast<void*>(owner))",
        "command.nitro_activations",
        "action_calls_submitted",
        "kActionSubmissionFailure",
        "calls_submitted,\n                false",
        "CompleteNaturalCallback",
    ):
        require(token in source_text, f"scheduler source token missing: {token}")
    for forbidden in (
        "NitroService", "kServiceActivateRva", "active_188", "mode_18c",
        "pthread_create", "socket(", "ptrace(", "pwrite(", "mprotect(",
    ):
        require(forbidden not in source_text,
                f"scheduler payload contains forbidden route: {forbidden}")

    require('"-DA9TAS_NAL_ACTION_EXECUTE=1"' in build_text,
            "action review compile gate missing")
    require("device_access=0" in build_text and "adb" not in build_text.lower(),
            "action review build must remain device-free")

    symbols = output(str(readelf), "--dyn-syms", "--wide", str(action))
    for name in (
        "a9tas_natural_action_registration_bootstrap_v1",
        "a9tas_natural_action_persistent_consumer_v1",
        "a9tas_natural_action_lifecycle_arm_v1",
        "a9tas_natural_action_lifecycle_control_data_v1",
        "a9tas_natural_action_lifecycle_evidence_data_v1",
        "a9tas_natural_action_lifecycle_mailbox_data_v1",
    ):
        require(re.search(rf"\b{re.escape(name)}$", symbols, re.MULTILINE),
                f"action payload export missing: {name}")
    for name in ("ptrace", "pwrite", "mprotect", "socket", "pthread_create", "dlopen"):
        require(re.search(rf"UND\s+{name}(?:@|$)", symbols, re.MULTILINE) is None,
                f"action payload imports forbidden primitive: {name}")

    disasm = output(str(objdump), "-d", "--demangle", str(action))
    require("SubmitGameOwnedActions" in disasm,
            "scheduler submission body missing from action payload")
    submit = re.search(
        r"<\(anonymous namespace\)::SubmitGameOwnedActions\(.*?>:\n"
        r"(.*?)(?=\n[0-9a-f]+ <)", disasm, re.DOTALL,
    )
    submit_text = submit.group(1).lower() if submit else ""
    require(submit is not None and
            len(re.findall(r"\bblr\t", submit_text)) == 2,
            "scheduler body must contain exactly two bounded action call sites")
    require(submit_text.count("#0x1378") >= 2 and
            submit_text.count("#0x1360") >= 2 and "#0x7ee" in submit_text,
            "scheduler disassembly lost direct-mode, queue or vtable guards")
    arm = re.search(
        r"<a9tas_natural_action_lifecycle_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL,
    )
    arm_text = arm.group(1).lower() if arm else ""
    require(arm is not None and
            ("mov\tw0, #-100" in arm_text or
             ("mov\tw0, #-0x64" in arm_text and "=-100" in arm_text)),
            "action review arm export is not constant -100")

    print(
        "NATURAL_ACTION_SCHEDULER_PAYLOAD_AUDIT passed=1 "
        "game_scheduler=1 activations_0_1_2=1 direct_mode_zero=1 "
        "token_growth_exact=1 direct_nitro_state=0 arm_rejected=1 "
        "deployed=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
