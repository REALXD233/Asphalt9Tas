#!/usr/bin/env python3
"""Android artifact policy for the A9AST1 runtime candidate."""
from __future__ import annotations
import pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "android-port/src/hwbp_authoritative_steering_v1.cpp"

def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout

def require(value: bool, message: str) -> None:
    if not value: raise RuntimeError(message)

def main() -> int:
    if len(sys.argv) != 5:
        print("usage: policy CANDIDATE OBJECT READELF OBJDUMP", file=sys.stderr); return 2
    candidate, obj, readelf, objdump = map(pathlib.Path, sys.argv[1:])
    for path in (candidate, obj, readelf, objdump, SOURCE): require(path.is_file(), f"missing: {path}")
    source = SOURCE.read_text(encoding="utf-8")
    require(source.count("pwrite(") == 2, "expected exactly two pwrite primitives")
    require(source.count("WriteFixedDeltaVerified(") == 2, "delta helper/call mismatch")
    require(source.count("WritePairVerified(") == 2, "pair helper/call mismatch")
    for token in ("kMinimumFrames = 1", "kMaximumFrames = 36000", "kRequiredFixedUs = 16667", "RejectSearchPrefix", "search_world_gap_recovery_failed", "PlanPair(", "report.pair_writes == required_frames * 2", "report.gameplay_action_calls == 0"):
        require(token in source, f"missing source contract: {token}")
    for token in ("process_vm_writev", "PTRACE_POKEDATA", "PTRACE_POKETEXT", "RemoteCall", "NitroEnable", "ApplySteering"):
        require(token not in source, f"forbidden capability: {token}")
    require("X86-64" in run(str(readelf), "-h", str(candidate)), "candidate is not Android x86_64")
    symbols = run(str(readelf), "-Ws", str(candidate))
    require("pwrite" in symbols and "ptrace" in symbols, "required runtime imports missing")
    for symbol in ("process_vm_writev", "socket", "connect"):
        require(symbol not in symbols, f"forbidden import: {symbol}")
    disasm = run(str(objdump), "-d", "--demangle", str(candidate))
    require(disasm.count("<pwrite@plt>") == 3, "expected two pwrite calls plus PLT label")
    for token in ("WriteFixedDeltaVerified", "WritePairVerified", "authoritative_natural_v1::Advance", "authoritative_steering_v1::PlanPair"):
        require(token in disasm, f"missing linked runtime component: {token}")
    require(" main" in run(str(readelf), "-Ws", str(obj)), "candidate main missing")
    print("AUTHORITATIVE_STEERING_CPP_POLICY passed=1 frames=recording_declared max_frames=36000 delta_calls=N pair_calls=2N search_writes=0 other_capabilities=absent deployed=0 device_access=0")
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as error:
        print(f"AUTHORITATIVE_STEERING_CPP_POLICY passed=0 error={error}", file=sys.stderr); raise SystemExit(1)
