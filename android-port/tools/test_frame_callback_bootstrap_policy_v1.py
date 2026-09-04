#!/usr/bin/env python3
"""Offline-only FC-0 vtable and payload policy proof."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_frame_callback_bootstrap_v1.cpp"
RAW_LIB = ROOT / "evidence" / "libAsphalt9_guest_20260816.so"

SECTION_VADDR = 0x7EDF608
SECTION_FILE_OFFSET = 0x7EDE608
PREFIX_RVA = 0x7EE8CC0
ADDRESS_POINT_RVA = 0x7EE8D18
NEXT_ADDRESS_POINT_RVA = 0x7EE95C8
PREFIX_SIZE = 0x58
SHADOW_SIZE = 0x908
CALLBACK_SLOT = 0x10
ORIGINAL_CALLBACK_RVA = 0x367D66C
SLICE_SHA256 = "0df7c204146ade7a2d4f4575da47d24a741bc2d317790a610a232a8c17c07094"
LIB_SHA256 = "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0"

EXPECTED_PREFIX = (
    0x17D8,
    0x17A8,
    0x1398,
    0x1778,
    0x13C8,
    0x1748,
    0x1748,
    0x13C8,
    0x1398,
    0,
    0,
)
EXPECTED_FIRST_SLOTS = (0x367AB34, 0x367AD84, 0x367D66C, 0x36818A0)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read_primary_slice() -> bytes:
    require(RAW_LIB.is_file(), f"raw guest library missing: {RAW_LIB}")
    require(hashlib.sha256(RAW_LIB.read_bytes()).hexdigest() == LIB_SHA256,
            "raw guest library hash mismatch")
    require(ADDRESS_POINT_RVA - PREFIX_RVA == PREFIX_SIZE, "prefix arithmetic")
    require(NEXT_ADDRESS_POINT_RVA - PREFIX_RVA == SHADOW_SIZE, "shadow arithmetic")
    file_offset = SECTION_FILE_OFFSET + (PREFIX_RVA - SECTION_VADDR)
    with RAW_LIB.open("rb") as stream:
        stream.seek(file_offset)
        data = stream.read(SHADOW_SIZE)
    require(len(data) == SHADOW_SIZE, "short primary vtable slice")
    return data


def verify_raw_vtable() -> None:
    data = read_primary_slice()
    require(hashlib.sha256(data).hexdigest() == SLICE_SHA256, "slice hash mismatch")
    prefix = struct.unpack_from("<11Q", data, 0)
    slots = struct.unpack_from("<4Q", data, PREFIX_SIZE)
    require(prefix == EXPECTED_PREFIX, f"primary prefix mismatch: {prefix!r}")
    require(slots == EXPECTED_FIRST_SLOTS, f"primary slots mismatch: {slots!r}")

    replacement = 0x1122334455667788
    shadow = bytearray(data)
    struct.pack_into("<Q", shadow, PREFIX_SIZE + CALLBACK_SLOT, replacement)
    changed = [index for index, pair in enumerate(zip(data, shadow)) if pair[0] != pair[1]]
    require(changed, "shadow replacement changed no bytes")
    require(min(changed) >= PREFIX_SIZE + CALLBACK_SLOT, "change before callback slot")
    require(max(changed) < PREFIX_SIZE + CALLBACK_SLOT + 8, "change after callback slot")
    require(shadow[: PREFIX_SIZE + CALLBACK_SLOT] == data[: PREFIX_SIZE + CALLBACK_SLOT],
            "prefix or earlier slot changed")
    require(shadow[PREFIX_SIZE + CALLBACK_SLOT + 8 :] ==
            data[PREFIX_SIZE + CALLBACK_SLOT + 8 :], "tail changed")


def verify_source_policy() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    required = {
        "prefix RVA": "kPrimaryPrefixRva = 0x7EE8CC0",
        "address point RVA": "kPrimaryAddressPointRva = 0x7EE8D18",
        "next address point RVA": "kNextAddressPointRva = 0x7EE95C8",
        "callback RVA": "kOriginalCallbackRva = 0x367D66C",
        "prefix size": "kPrimaryPrefixSize = 0x58",
        "shadow size": "kPrimaryShadowSize = 0x908",
        "slot offset": "kCallbackSlotOffset = 0x10",
        "build-only rejection": "return kStatusRejectedBuildOnly;",
    }
    for label, needle in required.items():
        require(needle in text, f"missing {label}")

    forbidden = (
        "ptrace(", "process_vm_writev", "pwrite(", "mprotect(", "dlopen(",
        "pthread_create", "socket(", "connect(", "accept(", "send(", "recv(",
        "CarPhysicsState_dispatch_action_158", "Nitro", "fixed_delta",
    )
    for needle in forbidden:
        require(needle not in text, f"forbidden live/action primitive: {needle}")

    wrapper = re.search(
        r"a9tas_fc0_frame_callback_passthrough_v1\(.*?\n}\n\nextern \"C\"",
        text,
        re.DOTALL,
    )
    require(wrapper is not None, "wrapper body not found")
    body = wrapper.group(0)
    restore = body.index("__atomic_store_n(object_vptr, original_vptr")
    evidence = body.index("g_evidence.wrapper_entries")
    original_call = body.index("original(object, frame_token)")
    require(restore < evidence < original_call,
            "vptr must restore before evidence and original call")
    require(body.count("original(object, frame_token)") == 1,
            "original callback must have one call site")
    require("return result;" in body, "original result must propagate")
    require("g_wrapper_depth.fetch_add" in body and "g_wrapper_depth.fetch_sub" in body,
            "recursion depth guard missing")
    require("if (control_complete && observed_vptr == shadow_vptr)" in body,
            "vptr restoration is not conditional on the exact FC-1 shadow")
    unexpected_object = body.index(
        "reinterpret_cast<std::uintptr_t>(object) != expected_object"
    )
    require(restore < unexpected_object,
            "unexpected-object path can return before restoring the FC-1 shadow")
    require("observed_vptr != shadow_vptr && observed_vptr != original_vptr" in body,
            "unexpected third-party vptr guard missing")


def verify_behavioral_contract() -> None:
    events: list[str] = []
    original_vptr = 0x1000
    shadow_vptr = 0x2000
    object_state = {"vptr": shadow_vptr}
    token = object()
    calls = 0

    def original(passed_object: dict[str, int], passed_token: object) -> int:
        nonlocal calls
        calls += 1
        require(passed_object is object_state, "object identity changed")
        require(passed_token is token, "token identity changed")
        require(passed_object["vptr"] == original_vptr,
                "original callback observed shadow vptr")
        events.append("original")
        return 0x5566770011223344

    observed = object_state["vptr"]
    require(observed == shadow_vptr, "model did not begin at shadow vptr")
    object_state["vptr"] = original_vptr
    events.append("restore")
    events.append("evidence")
    result = original(object_state, token)
    events.append("return")
    require(events == ["restore", "evidence", "original", "return"],
            f"behavioral order mismatch: {events!r}")
    require(calls == 1, "original callback count mismatch")
    require(result == 0x5566770011223344, "result propagation mismatch")
    require(object_state["vptr"] == original_vptr, "vptr not left restored")


def verify_arm64_artifact(
    path: pathlib.Path, readelf: pathlib.Path, objdump: pathlib.Path
) -> None:
    require(path.is_file(), f"artifact missing: {path}")
    header = subprocess.check_output([str(readelf), "-h", str(path)], text=True)
    require(re.search(r"Machine:\s+AArch64", header) is not None, "artifact is not AArch64")
    dynamic = subprocess.check_output([str(readelf), "-d", str(path)], text=True)
    require("libc++_shared.so" not in dynamic, "dynamic libc++ dependency")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(path)], text=True
    )
    exports = (
        "a9tas_fc0_frame_callback_passthrough_v1",
        "a9tas_fc0_frame_callback_protocol_v1",
        "a9tas_fc0_frame_callback_arm_v1",
        "a9tas_fc0_frame_callback_shadow_storage_v1",
        "a9tas_fc0_frame_callback_shadow_size_v1",
        "a9tas_fc0_frame_callback_control_storage_v1",
        "a9tas_fc0_frame_callback_evidence_storage_v1",
        "a9tas_fc0_frame_callback_primary_prefix_rva_v1",
        "a9tas_fc0_frame_callback_primary_address_point_rva_v1",
        "a9tas_fc0_frame_callback_next_address_point_rva_v1",
        "a9tas_fc0_frame_callback_original_callback_rva_v1",
        "a9tas_fc0_frame_callback_shadow_storage_data_v1",
        "a9tas_fc0_frame_callback_control_storage_data_v1",
        "a9tas_fc0_frame_callback_evidence_storage_data_v1",
        "a9tas_fc0_frame_callback_control_size_data_v1",
        "a9tas_fc0_frame_callback_evidence_size_data_v1",
    )
    for name in exports:
        require(name in symbols, f"missing export: {name}")
    for name in ("ptrace", "process_vm_writev", "pwrite", "mprotect", "dlopen",
                 "pthread_create", "socket", "connect", "accept", "send", "recv"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", symbols) is None,
                f"forbidden dynamic import: {name}")

    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(path)], text=True
    )
    match = re.search(
        r"<a9tas_fc0_frame_callback_passthrough_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(match is not None, "wrapper disassembly missing")
    body = match.group(1)
    restore = re.search(r"\bstlr\s+x\d+, \[x0\]", body)
    original = re.search(r"\bblr\s+x\d+", body)
    require(restore is not None and original is not None,
            "wrapper restore or original indirect call missing")
    require(restore.start() < original.start(),
            "AArch64 original call precedes vptr restoration")
    before_call = body[max(0, original.start() - 220): original.start()]
    require(re.search(r"\bmov\s+x0, x\d+", before_call) is not None,
            "wrapper does not restore object argument in x0")
    require(re.search(r"\bmov\s+x1, x\d+", before_call) is not None,
            "wrapper does not restore token argument in x1")

    arm = re.search(
        r"<a9tas_fc0_frame_callback_arm_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        disassembly,
        re.DOTALL,
    )
    require(arm is not None and "#-0x64" in arm.group(1) and "\tret" in arm.group(1),
            "build-only arm entry does not compile to constant -100")


def main() -> int:
    verify_raw_vtable()
    verify_source_policy()
    verify_behavioral_contract()
    if len(sys.argv) == 4:
        verify_arm64_artifact(
            pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
        )
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [artifact llvm-readelf llvm-objdump]")
    print("FC0_POLICY passed=1 raw_lib=1 raw_slice=1 single_slot=1 build_only=1 "
          "wrapper_order=1 behavioral_contract=1 arm64_abi=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
