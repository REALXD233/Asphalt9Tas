#!/usr/bin/env python3
from __future__ import annotations
import pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "android-port/src/authoritative_natural_handoff_v1.cpp"

def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout

def require(value: bool, message: str) -> None:
    if not value: raise RuntimeError(message)

def main() -> int:
    if len(sys.argv) != 6:
        print("usage: policy PASSIVE SELFTEST OBJECT READELF OBJDUMP", file=sys.stderr); return 2
    passive, selftest, obj, readelf, objdump = map(pathlib.Path, sys.argv[1:])
    for path in (passive, selftest, obj, readelf, objdump, SOURCE):
        require(path.is_file(), f"missing: {path}")
    text = SOURCE.read_text(encoding="utf-8")
    for token in ("state->mode = ModeV1::kReplay", "authoritative_bridge_v1::Advance", "tick0.replay.packet_selected", "search_cycles != 2"):
        require(token in text, f"missing contract: {token}")
    for token in ("ptrace(", "pwrite(", "pread(", "/proc/", "RemoteCall", "process_vm_"):
        require(token not in text, f"forbidden primitive: {token}")
    for binary in (passive, selftest):
        require("X86-64" in run(str(readelf), "-h", str(binary)), "not x86_64 ELF")
        symbols = run(str(readelf), "-Ws", str(binary))
        for symbol in ("ptrace", "pread", "pwrite", "waitpid", "kill", "socket"):
            require(symbol not in symbols, f"forbidden import: {symbol}")
    require("a9tas_authoritative_natural_handoff_selftest_v1" in run(str(readelf), "-Ws", str(selftest)), "selftest missing")
    disasm = run(str(objdump), "-d", "--demangle", str(obj))
    for token in ("authoritative_natural_v1::Create", "authoritative_natural_v1::Advance", "authoritative_natural_v1::Destroy"):
        require(token in disasm, f"API absent: {token}")
    print("AUTHORITATIVE_NATURAL_HANDOFF_CPP_POLICY passed=1 next_delta_tick0=1 runtime=disabled device_access=0 game_writes=0")
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as error:
        print(f"AUTHORITATIVE_NATURAL_HANDOFF_CPP_POLICY passed=0 error={error}", file=sys.stderr); raise SystemExit(1)
