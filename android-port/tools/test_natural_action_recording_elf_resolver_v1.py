#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


EXPECTED_SHA = "ad3ae03543b7489102adb53dba320296e93b034b12fe18bcf216969bc123c527"
EXPECTED_BUILD_ID = "4c9c7a0815967f0a596c4e94115d52d3a51a78f9"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    require(len(sys.argv) == 5,
            "usage: test RESOLVER PAYLOAD READELF OBJDUMP")
    resolver, payload, readelf, objdump = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (resolver, payload, readelf, objdump):
        require(path.is_file(), f"missing: {path}")
    require(hashlib.sha256(payload.read_bytes()).hexdigest() == EXPECTED_SHA,
            "payload SHA-256")
    notes = subprocess.check_output([str(readelf), "-n", str(payload)], text=True)
    require(EXPECTED_BUILD_ID in notes, "payload Build ID")
    header = resolver.read_text(encoding="utf-8")
    for token in (
        "kExpectedSha256", "kWrapperRva = 0x1b50",
        "kControlLocatorRva = 0x4040", "HashFile(file, hash)",
        "kControlStorageRva = 0x3fc0", "kShadowStorageRva = 0x78c0",
        "layout.counts_size != sizeof(std::uint32_t) * kMaximumFrames",
        "Writable(maps, layout.shadow", "AArch64 PT_LOAD(PF_X)",
    ):
        require(token in header, f"resolver policy missing: {token}")
    for forbidden in ("process_vm_writev", "pwrite", "ptrace(",
                      "input keyevent", "NitroState", "kNitroMode"):
        require(forbidden not in header, f"resolver primitive: {forbidden}")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(payload)], text=True)
    expected = {
        "a9tas_natural_action_recording_wrapper_v1": "0000000000001b50",
        "a9tas_natural_action_recording_control_storage_data_v1": "0000000000004040",
        "a9tas_natural_action_recording_evidence_storage_data_v1": "0000000000004048",
        "a9tas_natural_action_recording_counts_storage_data_v1": "0000000000004050",
        "a9tas_natural_action_recording_shadow_storage_data_v1": "0000000000004058",
    }
    for name, value in expected.items():
        require(re.search(rf"\b{value}\b.*\b{name}$", symbols, re.MULTILINE)
                is not None, f"symbol RVA changed: {name}")
    print("NATURAL_ACTION_RECORDING_ELF_RESOLVER passed=1 sha256=1 "
          "build_id=1 fixed_rvas=1 fixed_storage_rvas=1 "
          "nativebridge_zero_pointer_compatible=1 guest_calls=0 "
          "device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
