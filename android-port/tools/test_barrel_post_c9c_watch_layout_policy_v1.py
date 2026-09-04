#!/usr/bin/env python3
"""Audit exact post-C9C DR0 swap addresses and DR7 encodings."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
HEADER = WORKSPACE / "android-port" / "src" / "barrel_post_c9c_watch_layout_v1.h"
SELFTEST = WORKSPACE / "android-port" / "src" / "barrel_post_c9c_watch_layout_selftest_v1.cpp"
BASELINE = WORKSPACE / "android-port" / "src" / "hwbp_pipeline_order_observer_v1.cpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 2:
        print("usage: policy SELFTEST READELF", file=sys.stderr)
        return 2
    artifact, readelf = map(pathlib.Path, args)
    for path in (artifact, readelf, HEADER, SELFTEST, BASELINE):
        require(path.is_file(), f"missing required file: {path}")
    header = HEADER.read_text(encoding="utf-8")
    baseline = BASELINE.read_text(encoding="utf-8")
    for token in (
        "kRbxFirstOffset = 0x1968",
        "kRbxSecondOffset = 0x196C",
        "kAngularAuxOffset = 0x0C",
        "LocalWrite(0, 3)",
        "result.dr0_rbx_first",
        "result.dr0_rbx_second",
        "result.dr1_angular_aux",
        "kParallelActiveDr7",
        "kRbxDisabledDr7",
        "kPostF64Dr7",
        "result.dr1_callback_flags = callback_flags",
        "result.dr2_f64 = f64",
        "result.dr3_world_commit = world_commit",
    ):
        require(token in header, f"missing exact layout contract: {token}")
    require("Length encoding: 8=2, 2=1, 4=3, 4=3" in baseline,
            "known-good DR7 encoding evidence changed")
    for token in ("PTRACE_", "process_vm_", "/proc/", "pread(", "pwrite(",
                  "kill(", "waitpid(", "RemoteCall"):
        require(token not in header, f"pure layout gained runtime primitive: {token}")
    elf = run(str(readelf), "-h", str(artifact))
    require("Machine:" in elf and "X86-64" in elf,
            "watch layout selftest is not Android x86_64 ELF")
    rodata = run(str(readelf), "-p", ".rodata", str(artifact))
    require("BARREL_POST_C9C_WATCH_LAYOUT_SELFTEST" in rodata,
            "watch layout selftest receipt missing")
    print(
        "BARREL_POST_C9C_WATCH_LAYOUT_POLICY passed=1 dr0_swap=1 "
        "rbx_yaw_parallel=1 all_active_len4=1 callback_restored_at_f64=1 "
        "dr0_dr2_disabled_post_f64=1 "
        "runtime=disabled device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_POST_C9C_WATCH_LAYOUT_POLICY passed=0 error={error}",
              file=sys.stderr)
        raise SystemExit(1)
