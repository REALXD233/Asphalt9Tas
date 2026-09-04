#!/usr/bin/env python3
"""Static safety policy for the guarded zero-call lifecycle transaction."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "natural_action_lifecycle_transaction_v1.h"
SELFTEST = ROOT / "src" / "natural_action_lifecycle_transaction_selftest_v1.cpp"
BUILD = ROOT / "build-natural-action-lifecycle-transaction-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    selftest = SELFTEST.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for token in (
        "kReadyNoAttach", "kForceStopRequired", "mutation_started",
        "game_build_identity", "payload_elf_identity", "fixed_environment",
        "esc_count != 1", "first_positive_delta", "game_write_count != 1",
        "proof.bootstrap_entries == 1", "proof.original_calls == 1",
        "proof.registration_attempts == 1", "proof.list_end_before + 16u",
        "proof.original_vptr_restored", "AcceptRegisteredDetach",
        "command->nitro_activations != 0", "CancelBeforeC98",
        "MarkPublishedAtC98", "AcceptZeroReceiptAndCommit",
        "state->action_state.host_gate.pending", "PrepareCleanRemoval",
        "proof.removal_attempts == 1", "proof.object_absent",
        "proof.callback_count_stable", "AcceptCleanEnd",
    ):
        require(token in header, f"transaction contract missing: {token}")
    for forbidden in (
        "ptrace", "pwrite", "process_vm_writev", "/proc/", "adb",
        "NativeBridge", "dlopen", "mprotect", "system(", "CreateProcess",
    ):
        require(forbidden.lower() not in header.lower(),
                f"process primitive in pure transaction: {forbidden}")
    require("nonzero.nitro_activation_count = 1" in selftest and
            "kForceStopRequired" in selftest,
            "selftest must cover nonzero rejection after mutation")
    require("x86_64-linux-android24-clang++.cmd" in build and
            '"-Wall" "-Wextra" "-Werror"' in build,
            "build must compile strict x86-64 selftest")
    require("runtime=disabled" in build and "device_access=0" in build and
            "adb" not in build.lower(),
            "transaction build must remain offline")
    print("NATURAL_ACTION_LIFECYCLE_TRANSACTION_POLICY passed=1 "
          "startline=1 exact_registration=1 zero_call_only=1 "
          "clean_remove=1 force_stop_on_uncertain_cleanup=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
