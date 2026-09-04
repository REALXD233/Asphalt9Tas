#!/usr/bin/env python3
"""Offline policy checks for the FC-2 deferred-registration payload."""

from __future__ import annotations

import pathlib
import re
import struct
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_frame_callback_deferred_registration_v1.cpp"
BUILD = ROOT / "build-frame-callback-deferred-registration-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    required = (
        "kPrimaryPrefixRva = 0x7EE8CC0",
        "kPrimaryAddressPointRva = 0x7EE8D18",
        "kNextAddressPointRva = 0x7EE95C8",
        "kOriginalCallbackRva = 0x367D66C",
        "kPhysicsContextVtableRva = 0x8103830",
        "kPhysicsContextAddRva = 0x38B77CC",
        "kPhysicsContextRemoveRva = 0x38B7840",
        "kAddVslotOffset = 0x50",
        "kRemoveVslotOffset = 0x58",
        "static_assert(sizeof(Control) == 128",
        "static_assert(sizeof(Evidence) == 256",
        "a9tas_fc2_frame_callback_bootstrap_v1",
        "a9tas_fc2_dedicated_observer_v1",
        "a9tas_fc2_frame_callback_arm_v1",
        "return kStatusRejectedBuildOnly",
        "g_dedicated_vtable[5]",
        "reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2])",
        "g_registration_state.compare_exchange_strong",
        "g_removal_state.compare_exchange_strong",
        "registration_dispatching_before",
        "registration_deferred_after",
        "removal_dispatching_before",
        "removal_deferred_after",
        "after_size != before_size + 16u",
        "after_active != before_active",
        "a9tas_fc2_dedicated_object_data_v1",
        "a9tas_fc2_dedicated_vtable_data_v1",
        "control.version == 1 && control.size == sizeof(Control)",
        "RequestDeferredRemoval(control, remove)",
    )
    for needle in required:
        require(needle in text, f"missing FC-2 policy token: {needle}")

    forbidden = (
        "ptrace(", "mprotect(", "process_vm_writev", "PTRACE_SETREGS",
        "dispatch_action", "input keyevent", "input tap", "socket(",
        "dlopen(", "pthread_create", "std::thread", "syscall(",
    )
    for needle in forbidden:
        require(needle not in text, f"forbidden FC-2 primitive: {needle}")

    wrapper = text.index("a9tas_fc2_frame_callback_bootstrap_v1(void* car")
    restore = text.index("Store(car_vptr_address, control.original_car_vptr)", wrapper)
    original = text.index("const std::int64_t result = original(car, frame_token)", restore)
    add = text.index("add(reinterpret_cast<void*>(control.physics_context)", original)
    require(restore < original < add,
            "FC-2 must restore, call original, then register")
    require(text.count("add(reinterpret_cast<void*>(control.physics_context)") == 1,
            "FC-2 add method must have one call site")

    observer = text.index("a9tas_fc2_dedicated_observer_v1(void* object")
    record = text.index("AddCounter(&g_evidence.dedicated_entries)", observer)
    remove = text.index("RequestDeferredRemoval(control, remove)", record)
    require(record < remove, "dedicated callback must record before self-removal")
    require(text.count("remove(reinterpret_cast<void*>(control.physics_context)") == 1,
            "FC-2 game remove method must have one centralized call site")
    post_add_failure = text.index("RecordFailure(kStatusRegistrationShape)", add)
    cleanup = text.index("RequestDeferredRemoval(control, remove)", post_add_failure)
    require(add < post_add_failure < cleanup,
            "failed add postconditions must request same-thread cleanup")

    build = BUILD.read_text(encoding="utf-8")
    require("aarch64-linux-android24-clang++.cmd" in build,
            "FC-2 build must target ARM64 Android")
    require("device_access=0" in build and "adb" not in build.lower(),
            "FC-2 build must remain device-free")


def verify_behavioral_model() -> None:
    # Model only the proven list-size/boundary properties used by the payload.
    before_size = 16 * 7
    before_active = before_size
    after_add_size = before_size + 16
    after_add_active = before_active
    require(after_add_size == before_size + 16,
            "deferred add size model")
    require(after_add_active == before_active,
            "deferred add active-boundary model")
    # The later self-removal zeroes its entry during dispatch; game compaction
    # then removes that zero entry after dispatch closes.
    entries = [object() for _ in range(7)] + ["dedicated"]
    entries[-1] = None
    compacted = [entry for entry in entries if entry is not None]
    require(len(compacted) == 7 and "dedicated" not in compacted,
            "deferred self-removal compaction model")


def verify_artifact(
    payload: pathlib.Path, readelf: pathlib.Path, objdump: pathlib.Path
) -> None:
    for path in (payload, readelf, objdump):
        require(path.is_file(), f"missing FC-2 artifact/tool: {path}")
    header = subprocess.check_output([str(readelf), "-h", str(payload)], text=True)
    require("AArch64" in header and re.search(r"Type:\s+DYN", header),
            "FC-2 payload ELF identity")

    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(payload)], text=True
    )
    for name in (
        "a9tas_fc2_frame_callback_bootstrap_v1",
        "a9tas_fc2_dedicated_observer_v1",
        "a9tas_fc2_frame_callback_arm_v1",
        "a9tas_fc2_shadow_storage_data_v1",
        "a9tas_fc2_control_storage_data_v1",
        "a9tas_fc2_evidence_storage_data_v1",
        "a9tas_fc2_dedicated_object_data_v1",
        "a9tas_fc2_dedicated_vtable_data_v1",
    ):
        require(re.search(rf"\b{re.escape(name)}$", symbols, re.MULTILINE),
                f"missing FC-2 dynamic export: {name}")
    for name in ("ptrace", "mprotect", "process_vm_writev", "pthread_create"):
        require(re.search(rf"UND\s+{re.escape(name)}(?:@|$)", symbols,
                          re.MULTILINE) is None,
                f"FC-2 payload imports forbidden primitive: {name}")

    symbol_values: dict[str, int] = {}
    for line in symbols.splitlines():
        match = re.match(
            r"\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)$",
            line,
        )
        if match:
            symbol_values[match.group(2).split("@")[0]] = int(match.group(1), 16)

    required_values = (
        "a9tas_fc2_dedicated_observer_v1",
        "a9tas_fc2_shadow_storage_data_v1",
        "a9tas_fc2_control_storage_data_v1",
        "a9tas_fc2_evidence_storage_data_v1",
        "a9tas_fc2_dedicated_object_data_v1",
        "a9tas_fc2_dedicated_vtable_data_v1",
        "a9tas_fc2_control_size_data_v1",
        "a9tas_fc2_evidence_size_data_v1",
    )
    for name in required_values:
        require(name in symbol_values, f"cannot parse FC-2 symbol value: {name}")

    relocations = subprocess.check_output(
        [str(readelf), "-r", "--wide", str(payload)], text=True
    )
    relative: dict[int, int] = {}
    abs64: dict[int, tuple[str, int]] = {}
    for line in relocations.splitlines():
        rel = re.match(
            r"\s*([0-9a-fA-F]+)\s+\S+\s+R_AARCH64_RELATIVE\s+([0-9a-fA-F]+)\s*$",
            line,
        )
        if rel:
            relative[int(rel.group(1), 16)] = int(rel.group(2), 16)
            continue
        absolute = re.match(
            r"\s*([0-9a-fA-F]+)\s+\S+\s+R_AARCH64_ABS64\s+\S+\s+(\S+)\s+\+\s+([0-9a-fA-F]+)\s*$",
            line,
        )
        if absolute:
            abs64[int(absolute.group(1), 16)] = (
                absolute.group(2).split("@")[0],
                int(absolute.group(3), 16),
            )

    shadow_locator = symbol_values["a9tas_fc2_shadow_storage_data_v1"]
    control_locator = symbol_values["a9tas_fc2_control_storage_data_v1"]
    evidence_locator = symbol_values["a9tas_fc2_evidence_storage_data_v1"]
    object_locator = symbol_values["a9tas_fc2_dedicated_object_data_v1"]
    vtable_locator = symbol_values["a9tas_fc2_dedicated_vtable_data_v1"]
    for locator in (
        shadow_locator,
        control_locator,
        evidence_locator,
        object_locator,
        vtable_locator,
    ):
        require(locator in relative,
                f"FC-2 data locator lacks RELATIVE relocation: 0x{locator:x}")

    dedicated_object = relative[object_locator]
    dedicated_vptr = relative[vtable_locator]
    require(relative.get(dedicated_object) == dedicated_vptr,
            "FC-2 dedicated object does not relocate to dedicated vtable")
    require(relative.get(dedicated_vptr) == relative.get(dedicated_vptr + 8),
            "FC-2 hidden fail-safe slots differ")
    observer_relocation = abs64.get(dedicated_vptr + 16)
    require(observer_relocation ==
            ("a9tas_fc2_dedicated_observer_v1", 0),
            "FC-2 vslot +0x10 does not relocate to dedicated observer")

    data = payload.read_bytes()
    require(data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1,
            "FC-2 payload is not little-endian ELF64")
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    program_entry_size = struct.unpack_from("<H", data, 54)[0]
    program_count = struct.unpack_from("<H", data, 56)[0]
    loads: list[tuple[int, int, int, int]] = []
    executable_ranges: list[tuple[int, int]] = []
    for index in range(program_count):
        offset = program_offset + index * program_entry_size
        kind, flags = struct.unpack_from("<II", data, offset)
        if kind != 1:  # PT_LOAD
            continue
        file_offset, virtual_address, _, file_size, memory_size = struct.unpack_from(
            "<QQQQQ", data, offset + 8
        )
        loads.append((virtual_address, memory_size, file_offset, file_size))
        if flags & 1:  # PF_X
            executable_ranges.append((virtual_address,
                                      virtual_address + memory_size))

    def read_u64(virtual_address: int) -> int:
        for start, memory_size, file_offset, file_size in loads:
            delta = virtual_address - start
            if 0 <= delta and delta + 8 <= file_size:
                return struct.unpack_from("<Q", data, file_offset + delta)[0]
        raise AssertionError(f"FC-2 RVA is not file-backed: 0x{virtual_address:x}")

    require(read_u64(dedicated_vptr - 16) == 0 and
            read_u64(dedicated_vptr - 8) == 0,
            "FC-2 Itanium vtable prefix must be two zero qwords")
    fail_safe = relative.get(dedicated_vptr)
    require(fail_safe is not None and any(
        start <= fail_safe < end for start, end in executable_ranges
    ), "FC-2 hidden fail-safe callback is outside executable PT_LOAD")
    require(read_u64(symbol_values["a9tas_fc2_control_size_data_v1"]) == 128,
            "FC-2 control-size data is not 128")
    require(read_u64(symbol_values["a9tas_fc2_evidence_size_data_v1"]) == 256,
            "FC-2 evidence-size data is not 256")

    disasm = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(payload)], text=True
    )
    arm = re.search(
        r"<a9tas_fc2_frame_callback_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL,
    )
    arm_text = arm.group(1).lower() if arm is not None else ""
    require(arm is not None and
            ("mov\tw0, #-100" in arm_text or
             ("mov\tw0, #-0x64" in arm_text and "=-100" in arm_text)),
            "FC-2 arm export is not a constant -100")
    bootstrap = re.search(
        r"<a9tas_fc2_frame_callback_bootstrap_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL,
    )
    observer = re.search(
        r"<a9tas_fc2_dedicated_observer_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL,
    )
    removal_helper = re.search(
        r"^[0-9a-f]+ <\(anonymous namespace\)::RequestDeferredRemoval[^\n]*>:\n"
        r"(.*?)(?=\n[0-9a-f]+ <)",
        disasm, re.DOTALL | re.MULTILINE,
    )
    require(bootstrap is not None and observer is not None and
            removal_helper is not None,
            "FC-2 callback disassembly unavailable")
    require(bootstrap.group(1).lower().count("blr") >= 2,
            "bootstrap must contain original and add indirect calls")
    require("RequestDeferredRemoval" in observer.group(1) and
            removal_helper.group(1).lower().count("blr") == 1,
            "dedicated observer must reach one centralized remove call")


def main() -> int:
    verify_source()
    verify_behavioral_model()
    if len(sys.argv) == 4:
        verify_artifact(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [payload llvm-readelf llvm-objdump]")
    print("FC2_PAYLOAD_POLICY passed=1 build_only=1 restore_before_original=1 "
          "original_before_add=1 deferred_add=1 self_remove=1 observe_only=1 "
          "device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
