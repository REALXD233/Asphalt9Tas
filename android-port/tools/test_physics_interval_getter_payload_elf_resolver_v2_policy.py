#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("resolver", type=Path)
    args = parser.parse_args()
    text = args.resolver.read_text(encoding="utf-8")
    required = (
        "kExpectedSha256[32]",
        "kWrapperLocatorRva = 0x51E0",
        "kContinueLocatorRva = 0x5210",
        "HashFile(file, hash)",
        "mapping->path != path",
        "WritableRange",
        "control.magic",
        "evidence.magic",
    )
    forbidden = ("pwrite(", "process_vm_writev", "ptrace(", "O_RDWR", "mprotect(")
    failures = [f"missing marker: {item}" for item in required if item not in text]
    failures += [f"forbidden marker: {item}" for item in forbidden if item in text]
    if failures:
        print("\n".join(failures))
        return 1
    print("PHYSICS_INTERVAL_GETTER_ELF_RESOLVER_V2_POLICY passed=1 read_only=1 hash_pinned=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
