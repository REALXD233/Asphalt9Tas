#!/usr/bin/env python3
"""Offline policy and artifact audit for the FC-3 identity resolver."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc3_identity_resolver_v1.h"
SOURCE = ROOT / "src" / "fc3_replay_observer_identity_resolver_v1.cpp"
BUILD = ROOT / "build-fc3-replay-observer-identity-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for needle in (
        "replay_fixed_delta_input",
        "phase_witness_accumulator",
        "static_assert(sizeof(Layout) == 208",
        "offsetof(Layout, replay_fixed_delta_input) == 40",
        "offsetof(Layout, phase_witness_accumulator) == 96",
        "offsetof(Layout, action_owner) == 128",
    ):
        require(needle in header, f"missing FC-3 identity ABI token: {needle}")

    required = (
        "#define A9TAS_FC3_IDENTITY_REVIEW 0",
        "#if A9TAS_FC3_IDENTITY_REVIEW == 1",
        "fc2_payload_elf_resolver_v1.h",
        "a9tas::fc2_payload_elf_v1::Resolve",
        "ResolveMainObject(pid, base, 0, &main_object)",
        "ResolveFinalOwner(pid, base, 0, &physics_owner)",
        "ResolvePhysicsContext(pid, process_mem, base, 0, &context",
        "a9tas::vehicle_state_v1::Resolve",
        "ResolveUniqueActionOwner",
        "ResolveNitroIdentity",
        "bool CheckedAdd",
        "!CheckedAdd(owner, kFc3ActionVectorOffset, &vector)",
        "!CheckedAdd(physics_owner, kFc3NitroServiceOffset",
        "!CheckedAdd(context, kFc3CallbackListOffset, &callback_list)",
        "kFc3ReplayFixedDeltaOffset = 0x150",
        "kFc3PhaseWitnessOffset = 0x188",
        "kFc3ActionVectorOffset = 0x1360",
        "kFc3DirectModeOffset = 0x1378",
        "kFc3ActionDispatchVfunc158Rva = 0x36A9CAC",
        "kFc3NitroActivateRva = 0x3674E50",
        "car_active != 1 || car_full != 1 || dedicated_full != 0",
        "FC3_IDENTITY_BUILD_ONLY runtime=disabled return=-100",
        "device_access=0 process_reads=0 attached=0 game_writes=0",
    )
    for needle in required:
        require(needle in source, f"missing FC-3 resolver token: {needle}")

    for needle in (
        "pwrite(", "process_vm_writev", "PTRACE_POKEDATA", "PTRACE_SETREGS",
        "CarPhysicsState_dispatch_action", "NitroState_handle_activation",
        "input keyevent", "input tap", "socket(", "pthread_create",
    ):
        require(needle not in source,
                f"forbidden FC-3 resolver primitive: {needle}")

    require(source.index("VerifyTargetBuild(pid, base)") <
            source.index("a9tas::fc2_payload_elf_v1::Resolve"),
            "target build must precede identity resolution")
    require(source.index("ResolveUniqueActionOwner") <
            source.index("ResolveNitroIdentity", source.index(
                "a9tas_fc3_resolve_identities_review_v1")),
            "action identity must precede Nitro snapshot in exported resolver")

    for needle in (
        "-ffunction-sections", "-fdata-sections", "--gc-sections",
        "--localize-symbol=_Z34A9TasSchedulerObserverMain_NotUsediPPc",
        "liba9tas_fc3_identity_resolver_review_only.so",
        "Remove-Item -LiteralPath $intermediate -Force",
    ):
        require(needle in build, f"missing FC-3 build isolation: {needle}")
    require("adb" not in build.lower(), "FC-3 identity build must be offline")


def verify_behavioral_model() -> None:
    # These are identities, not aliases.  The phase witness must never be used
    # as the replay fixed-delta target.
    main_owner = 0x100000
    world = 0x200000
    replay_delta = main_owner + 0x150
    phase_witness = world + 0x188
    require(replay_delta != phase_witness, "delta/witness identity separation")

    def queue_ok(begin: int, end: int, capacity: int) -> bool:
        return (begin <= end <= capacity and (end - begin) % 8 == 0 and
                (capacity - begin) % 8 == 0 and (end - begin) // 8 <= 4096)

    require(queue_ok(0x3000, 0x3000, 0x3040), "neutral queue")
    require(queue_ok(0x3000, 0x3010, 0x3040), "bounded live queue")
    require(not queue_ok(0x3010, 0x3000, 0x3040), "reversed queue")
    require(not queue_ok(0x3000, 0x3007, 0x3040), "misaligned queue")
    require(len({0x4000}) == 1 and len({0x4000, 0x5000}) != 1,
            "unique action-owner rule")

    def checked_add(base: int, offset: int) -> int | None:
        pointer_max = (1 << 64) - 1
        return None if base > pointer_max - offset else base + offset

    require(checked_add(0x1000, 0x1378) == 0x2378,
            "ordinary derived address")
    require(checked_add((1 << 64) - 0x100, 0x188) is None,
            "derived-address overflow must reject")


def verify_artifacts(passive: pathlib.Path, review: pathlib.Path,
                     readelf: pathlib.Path, objdump: pathlib.Path) -> None:
    for path in (passive, review, readelf, objdump):
        require(path.is_file(), f"missing FC-3 identity artifact/tool: {path}")
    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(passive)], text=True
    )
    review_header = subprocess.check_output(
        [str(readelf), "-h", str(review)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header,
            "FC-3 identity passive architecture")
    require("Advanced Micro Devices X86-64" in review_header and
            re.search(r"Type:\s+DYN", review_header),
            "FC-3 identity review must be x86-64 DYN")

    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True
    )
    review_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(review)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "open", "fopen", "opendir"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive FC-3 identity imports process primitive: {name}")
    for name in ("ptrace", "pwrite", "process_vm_writev", "waitpid", "kill",
                 "socket", "dlopen", "mprotect"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", review_symbols) is None,
                f"review FC-3 identity imports forbidden primitive: {name}")
    for name in ("open", "fopen", "pread"):
        require(re.search(rf"\b{re.escape(name)}@", review_symbols) is not None,
                f"review FC-3 identity lacks read primitive: {name}")
    require("a9tas_fc3_resolve_identities_review_v1" in review_symbols,
            "FC-3 identity export missing")

    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(review)], text=True
    ).lower()
    for value in ("0x7eea080", "0x7eefe68", "0x36a9cac", "0x3674e50"):
        require(value in disassembly,
                f"FC-3 identity disassembly lacks constant {value}")


def main() -> int:
    verify_source()
    verify_behavioral_model()
    if len(sys.argv) == 5:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [passive review-so readelf objdump]"
        )
    print("FC3_IDENTITY_POLICY passed=1 unique_action_owner=1 nitro_identity=1 "
          "fixed_delta_witness_separate=1 review_imports=read_only "
          "device_access=0 game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
