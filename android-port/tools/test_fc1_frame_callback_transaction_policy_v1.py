#!/usr/bin/env python3
"""Offline FC-1 transaction and passive-artifact policy checks."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "fc1_frame_callback_transaction_controller_v1.cpp"
BUILD = ROOT / "build-fc1-frame-callback-transaction-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    required = (
        "#define A9TAS_FC1_LIVE_CANDIDATE 0",
        "#if A9TAS_FC1_LIVE_CANDIDATE == 1",
        "I_ACCEPT_FC1_ONE_FRAME_PASSTHROUGH_NO_ACTION_V1",
        "kCarPhysicsPrimaryVtable = 0x7EE8D18",
        "kPrimaryPrefix = 0x7EE8CC0",
        "kOriginalCallback = 0x367D66C",
        "kShadowSize = 0x908",
        "kCallbackListOffset = 0x180",
        "kCallbackFlagsOffset = 0x1A0",
        "kWrapperSignature[24]",
        "ResolveAndPinIdentity",
        "ValidatePinnedIdentity",
        "HasUniqueActiveObject",
        "FreezeAllExcept",
        "EvidenceComplete",
        "InstallShadowVptrWhileFrozen",
        "ConditionalRollback",
        "enum OpenRejectReason",
        "FC1_OPEN_REJECT reason=0x%x",
        "fc1_payload_elf_resolver_v1.h",
        "a9tas::fc1_payload_elf_v1::Resolve(pid, mem, &payload)",
        "MappingHas(wrapper_map, 'r') && !MappingHas(wrapper_map, 'w')",
        "live_slot != base + kExpectedSlots[index]",
        "FC1_BUILD_ONLY runtime=disabled return=-100 device_access=0",
    )
    for needle in required:
        require(needle in text, f"missing source policy token: {needle}")

    forbidden = (
        "adb ", "input keyevent", "input tap", "Nitro",
        "CarPhysicsState_dispatch_action_158", "PTRACE_SETREGS", "dlopen(",
        "mprotect(", "process_vm_writev",
    )
    for needle in forbidden:
        require(needle not in text, f"forbidden FC-1 primitive: {needle}")
    require("SHADOW_HEX" not in text and "CONTROL_HEX" not in text and
            "EVIDENCE_HEX" not in text and "WRAPPER_HEX" not in text,
            "payload addresses must come only from the ELF resolver")
    require("MappingHas(wrapper_map, 'x')" not in text,
            "Houdini guest wrapper must not require host executable permission")
    require("std::memcmp(table.data() + kPrefixSize, kExpectedSlots" not in text,
            "live relocated vtable slots must not be compared with raw RVAs")
    for token in (
        "kRejectUnexpectedWaitEvent", "kRejectWrongStopSignal",
        "kRejectDr6Read", "kRejectDr0NotSet", "kRejectFlagsRead",
        "kRejectThreadNameRead", "kRejectThreadNamePrefix",
        "kRejectMapsRead", "kRejectCallbackListRead",
        "kRejectDispatchNotOpen", "kRejectUniqueObject",
        "kRejectObjectVptrRead", "kRejectObjectVptrMismatch",
        "kRejectUnexpectedWaitState", "kRejectThreadSignaled",
    ):
        require(f"report.reserved0 |= {token}" in text,
                f"missing fail-closed open rejection diagnostic: {token}")
    require("if (WIFEXITED(status))" in text and
            "WEXITSTATUS(status) != 0" in text and
            "if (thread && thread->live)" in text and
            "++retired_threads" in text,
            "clean short-lived thread retirement policy must be explicit")
    require("detached + retired_threads" in text,
            "clean detach accounting must include known retired threads")
    require("WIFSIGNALED(status)" in text and
            "report.reserved0 |= kRejectThreadSignaled" in text,
            "signal-terminated threads must remain fail-closed")

    readonly = text.index("open(mem_path, O_RDONLY")
    resolve = text.index("ResolveAndPinIdentity", readonly)
    close_readonly = text.index("close(mem);", resolve)
    readwrite = text.index("open(mem_path, O_RDWR", close_readonly)
    prepare = text.index("PreparePayload", readwrite)
    attach = text.index("AttachCurrentThreads", prepare)
    require(readonly < resolve < close_readonly < readwrite < prepare < attach,
            "preflight/prepare/attach ordering changed")

    freeze = text.index("FreezeAllExcept(pid")
    swap = text.index("InstallShadowVptrWhileFrozen(mem, object", freeze)
    resume = text.index("ContinueThread(owner->tid)", swap)
    close_gate = text.index("kDispatchCloseSeen", resume)
    evidence = text.index("EvidenceComplete(report.evidence", close_gate)
    rollback = text.index("ConditionalRollback(mem, object", evidence)
    detach = text.index("RestoreAndDetachAll(&threads", rollback)
    require(freeze < swap < resume < close_gate < evidence < rollback < detach,
            "single-frame transaction ordering changed")
    require(text.count("InstallShadowVptrWhileFrozen(mem, object") == 1,
            "game shadow transaction must have one call site")
    require("written == static_cast<ssize_t>(sizeof(shadow_vptr))" in text,
            "shadow transaction must detect partial writes")
    require("WriteExactVerified(mem, object, &original_vptr" in text,
            "frozen partial-write rollback missing")
    require(text.count("WriteExactVerified(mem, object, &original_vptr") == 2,
            "expected frozen-failure and post-resume rollback call sites")
    require("if (current != shadow_vptr)" in text,
            "rollback must reject an unexpected third-party vptr")

    build = BUILD.read_text(encoding="utf-8")
    require('"-DA9TAS_FC1_LIVE_CANDIDATE=1" "-c"' in build,
            "live path must compile only to an unlinked review object")
    require("fc1_frame_callback_transaction_v1_live" not in build,
            "runnable live candidate name is forbidden")
    require("a9tas-fc1-linkcheck-" in build and
            "Remove-Item -LiteralPath $linkCheck -Force" in build and
            "link-check path escaped the system temp directory" in build,
            "ephemeral link validation/removal policy missing")
    require("adb" not in build.lower(), "build script must not access a device")


def verify_behavioral_model() -> None:
    def finish(ack: bool, close: bool, current: str) -> tuple[int, int, str, bool]:
        writes = 1
        rollbacks = 0
        if not (ack and close):
            if current == "shadow":
                current = "original"
                rollbacks += 1
            elif current != "original":
                return writes, rollbacks, current, False
        return writes, rollbacks, current, ack and close and current == "original"

    require(finish(True, True, "original") == (1, 0, "original", True),
            "success model")
    require(finish(False, True, "shadow") == (1, 1, "original", False),
            "missing acknowledgement rollback model")
    require(finish(True, False, "shadow") == (1, 1, "original", False),
            "frame timeout rollback model")
    require(finish(False, False, "third_party") ==
            (1, 0, "third_party", False), "no blind rollback model")


def verify_artifacts(
    passive: pathlib.Path, live_object: pathlib.Path,
    readelf: pathlib.Path, objdump: pathlib.Path, payload: pathlib.Path,
) -> None:
    for path in (passive, live_object, readelf, objdump, payload):
        require(path.is_file(), f"missing artifact/tool: {path}")
    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(passive)], text=True
    )
    object_header = subprocess.check_output(
        [str(readelf), "-h", str(live_object)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header,
            "passive controller architecture")
    require("Advanced Micro Devices X86-64" in object_header,
            "live review object architecture")
    require(re.search(r"Type:\s+REL \(Relocatable file\)", object_header) is not None,
            "live review artifact must be unlinked REL object")

    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "open", "waitpid", "kill"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive artifact imports device/process primitive: {name}")
    live_symbols = subprocess.check_output(
        [str(readelf), "--symbols", "--wide", str(live_object)], text=True
    )
    for name in ("ptrace", "pwrite", "pread", "waitpid"):
        require(re.search(rf"UND\s+{re.escape(name)}$", live_symbols,
                          re.MULTILINE) is not None,
                f"live review object did not compile transaction primitive: {name}")
    main_symbol = re.search(
        r"\s+[0-9a-f]+\s+(\d+)\s+FUNC\s+GLOBAL\s+DEFAULT\s+\d+\s+main$",
        live_symbols,
        re.MULTILINE,
    )
    require(main_symbol is not None and int(main_symbol.group(1)) > 8000,
            "live transaction main was optimized away or truncated")

    live_disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(live_object)], text=True
    )
    require("0x7ee8d18" in live_disassembly.lower(),
            "live review object lacks exact primary-vptr constant")
    require("$0x908" in live_disassembly.lower(),
            "live review object lacks exact shadow-size constant")

    payload_header = subprocess.check_output(
        [str(readelf), "-h", str(payload)], text=True
    )
    require("AArch64" in payload_header, "FC-0 payload architecture")
    payload_disassembly = subprocess.check_output(
        [str(objdump), "-d", str(payload)], text=True
    )
    wrapper = re.search(
        r"<a9tas_fc0_frame_callback_passthrough_v1>:\n(.*?)(?=\n[0-9a-f]+ <)",
        payload_disassembly,
        re.DOTALL,
    )
    require(wrapper is not None, "FC-0 wrapper disassembly unavailable")
    words = re.findall(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s", wrapper.group(1),
                       re.MULTILINE)[:6]
    actual_signature = b"".join(int(word, 16).to_bytes(4, "little") for word in words)
    source = SOURCE.read_text(encoding="utf-8")
    signature_block = re.search(
        r"kWrapperSignature\[24\]\s*=\s*\{(.*?)\};", source, re.DOTALL
    )
    require(signature_block is not None, "embedded wrapper signature missing")
    expected_signature = bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9a-fA-F]{2})", signature_block.group(1))
    )
    require(len(actual_signature) == 24 and actual_signature == expected_signature,
            "embedded wrapper signature differs from built FC-0 payload")


def main() -> int:
    verify_source()
    verify_behavioral_model()
    if len(sys.argv) == 6:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [passive live-object llvm-readelf llvm-objdump payload]"
        )
    print("FC1_POLICY passed=1 passive=1 readonly_pin=1 single_swap=1 "
          "frame_close_deadline=1 conditional_rollback=1 no_device=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
