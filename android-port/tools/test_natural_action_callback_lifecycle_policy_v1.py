#!/usr/bin/env python3
"""Policy and ELF checks for the build-only persistent action callback."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_natural_action_callback_lifecycle_v1.cpp"
CONTRACT = ROOT / "src" / "natural_action_callback_lifecycle_v1.h"
BUILD = ROOT / "build-natural-action-callback-lifecycle-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    contract = CONTRACT.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for token in (
        "#define A9TAS_NAL_ACTION_EXECUTE 0",
        "constexpr bool kActionExecutionCompiled = A9TAS_NAL_ACTION_EXECUTE == 1",
        "static_assert(!kActionExecutionCompiled",
        "a9tas_natural_action_registration_bootstrap_v1",
        "a9tas_natural_action_persistent_consumer_v1",
        "a9tas_natural_action_lifecycle_arm_v1",
        "return kRejectedBuildOnly",
        "g_bootstrap_state.compare_exchange_strong",
        "after.end == before.end + 16u",
        "after.active_end == before.active_end",
        "Load(&g_control.remove_requested) == 1",
        "control.remove_requested <= 1",
        "mailbox::LoadAcquire(&g_mailbox.claimed_sequence)",
        "mailbox::LoadAcquire(&g_mailbox.completed_sequence)",
        "command.nitro_activations != 0",
        "RequestRemoval(control, remove, true)",
        "static_assert(sizeof(Control) == 128",
        "static_assert(sizeof(Evidence) == 256",
    ):
        require(token in source, f"missing lifecycle source token: {token}")
    for forbidden in (
        "NitroService",
        "ptrace(",
        "pwrite(",
        "mprotect(",
        "socket(",
        "pthread_create",
        "dlopen(",
    ):
        require(forbidden not in source,
                f"forbidden lifecycle primitive: {forbidden}")

    bootstrap = source.index("a9tas_natural_action_registration_bootstrap_v1")
    restore = source.index("Store(car_vptr, control.original_car_vptr)", bootstrap)
    original = source.index("const std::int64_t result = original(car, frame_token)", restore)
    add = source.index("add(reinterpret_cast<void*>(control.physics_context)", original)
    require(restore < original < add,
            "bootstrap must restore, call original, then deferred-add")
    consumer = source.index("a9tas_natural_action_persistent_consumer_v1(", add)
    remove_flag = source.index("Load(&g_control.remove_requested) == 1", consumer)
    claim = source.index("ClaimAtNaturalCallback(", remove_flag)
    require(remove_flag < claim,
            "clean removal request must be handled before mailbox claim")

    for token in (
        "PrepareCleanRemoval",
        "claimed_sequence",
        "completed_sequence",
        "session_control",
        "MarkRemovalRequested",
        "AcceptRemoval",
        "object_absent",
        "callback_count_stable",
    ):
        require(token in contract, f"missing lifecycle contract token: {token}")
    for forbidden in ("ptrace", "pwrite", "/proc/", "socket(", "adb"):
        require(forbidden not in contract.lower(),
                f"lifecycle contract contains process primitive: {forbidden}")

    require("aarch64-linux-android24-clang++.cmd" in build and
            "x86_64-linux-android24-clang++.cmd" in build,
            "lifecycle build must compile ARM64 payload and x86 host selftest")
    require('"-Wall", "-Wextra", "-Werror"' in build,
            "lifecycle build must use warnings as errors")
    require("adb" not in build.lower() and "device_access=0" in build,
            "lifecycle build must remain device-free")


def verify_artifact(payload: pathlib.Path, readelf: pathlib.Path,
                    objdump: pathlib.Path) -> None:
    for path in (payload, readelf, objdump):
        require(path.is_file(), f"missing lifecycle artifact/tool: {path}")
    header = subprocess.check_output([str(readelf), "-h", str(payload)], text=True)
    require("AArch64" in header and re.search(r"Type:\s+DYN", header),
            "lifecycle payload is not AArch64 DSO")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(payload)], text=True
    )
    for name in (
        "a9tas_natural_action_registration_bootstrap_v1",
        "a9tas_natural_action_persistent_consumer_v1",
        "a9tas_natural_action_lifecycle_arm_v1",
        "a9tas_natural_action_lifecycle_shadow_data_v1",
        "a9tas_natural_action_lifecycle_control_data_v1",
        "a9tas_natural_action_lifecycle_evidence_data_v1",
        "a9tas_natural_action_lifecycle_mailbox_data_v1",
        "a9tas_natural_action_lifecycle_object_data_v1",
        "a9tas_natural_action_lifecycle_vtable_data_v1",
    ):
        require(re.search(rf"\b{re.escape(name)}$", symbols, re.MULTILINE),
                f"missing lifecycle export: {name}")
    for name in ("ptrace", "pwrite", "mprotect", "socket", "pthread_create", "dlopen"):
        require(re.search(rf"UND\s+{name}(?:@|$)", symbols, re.MULTILINE) is None,
                f"lifecycle imports forbidden primitive: {name}")
    disasm = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(payload)], text=True
    )
    require("367b414" not in disasm.lower(),
            "default lifecycle payload compiled the game action RVA")
    arm = re.search(
        r"<a9tas_natural_action_lifecycle_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL,
    )
    arm_text = arm.group(1).lower() if arm else ""
    require(arm is not None and
            ("mov\tw0, #-100" in arm_text or
             ("mov\tw0, #-0x64" in arm_text and "=-100" in arm_text)),
            "lifecycle arm export is not constant -100")


def main() -> int:
    verify_source()
    if len(sys.argv) == 4:
        verify_artifact(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [payload readelf objdump]")
    print("NATURAL_ACTION_CALLBACK_LIFECYCLE_POLICY passed=1 persistent=1 "
          "deferred_add_remove=1 execute_compiled=0 arm_rejected=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
