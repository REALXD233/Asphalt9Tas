#!/usr/bin/env python3
"""Cross-artifact audit for the complete local-only FC-2 live candidate."""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-fc2-frame-callback-v1.ps1"
BUILD = ROOT / "build-fc2-runner-v1.ps1"
BOOTSTRAP_SOURCE = ROOT / "src" / "bootstrap_frame_callback_fc2_v1_build.cpp"
HELPER = ROOT / "tools" / "run_fc2_frame_callback_preload_v1.sh"
VALIDATOR = ROOT / "tools" / "validate_fc2_report_v1.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def undefined_symbols(readelf: pathlib.Path, artifact: pathlib.Path) -> set[str]:
    text = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(artifact)], text=True
    )
    result = set()
    for line in text.splitlines():
        match = re.search(r"\bUND\s+(\S+)$", line)
        if match:
            result.add(match.group(1).split("@")[0])
    return result


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            f"usage: {sys.argv[0]} PAYLOAD CONTROLLER BOOTSTRAP READELF OBJDUMP"
        )
    payload, controller, bootstrap, readelf, objdump = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (
        payload, controller, bootstrap, readelf, objdump, RUNNER, BUILD,
        BOOTSTRAP_SOURCE, HELPER, VALIDATOR,
    ):
        require(path.is_file(), f"missing FC-2 audit input: {path}")

    runner = RUNNER.read_text(encoding="utf-8")
    pin_matches = dict(re.findall(
        r"\$(payload|bootstrap|controller|injector|helper|reportValidator)\s*=\s*"
        r'"([0-9a-f]{64})"', runner
    ))
    require(len(pin_matches) == 6, "runner pin map is incomplete")
    actual_pins = {
        "payload": sha256(payload),
        "controller": sha256(controller),
        "bootstrap": sha256(bootstrap),
        "injector": sha256(ROOT / "build" / "a9tas_injector"),
        "helper": sha256(HELPER),
        "reportValidator": sha256(VALIDATOR),
    }
    require(pin_matches == actual_pins, "runner pins differ from actual artifacts")

    headers = {
        name: subprocess.check_output(
            [str(readelf), "-h", str(path)], text=True
        )
        for name, path in {
            "payload": payload, "controller": controller,
            "bootstrap": bootstrap,
        }.items()
    }
    require("AArch64" in headers["payload"] and
            re.search(r"Type:\s+DYN", headers["payload"]),
            "payload must be AArch64 DYN")
    for name in ("controller", "bootstrap"):
        require("Advanced Micro Devices X86-64" in headers[name] and
                re.search(r"Type:\s+DYN", headers[name]),
                f"{name} must be x86-64 DYN")

    controller_undefined = undefined_symbols(readelf, controller)
    for name in ("open", "pread", "pwrite", "waitpid", "ptrace", "kill"):
        require(name in controller_undefined,
                f"controller transaction primitive missing: {name}")
    for name in (
        "socket", "connect", "system", "execve", "dlopen", "dlsym",
        "mprotect", "process_vm_writev", "pthread_create",
    ):
        require(name not in controller_undefined,
                f"controller imports forbidden primitive: {name}")

    bootstrap_undefined = undefined_symbols(readelf, bootstrap)
    for name in ("dlopen", "dlsym", "mprotect", "pthread_create"):
        require(name in bootstrap_undefined,
                f"preload bootstrap loader primitive missing: {name}")
    bootstrap_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(bootstrap)], text=True
    )
    for name in ("a9tas_bootstrap_status", "a9tas_bootstrap_stage"):
        require(re.search(rf"\b{re.escape(name)}$", bootstrap_symbols,
                          re.MULTILINE),
                f"bootstrap status export missing: {name}")

    expected_payload = (
        b"/data/local/tmp/"
        b"liba9tas_frame_callback_deferred_registration_v1_build_only.so"
    )
    require(expected_payload in bootstrap.read_bytes(),
            "bootstrap binary lacks exact FC-2 payload path")
    require(b"liba9tas_frame_callback_bootstrap_v1_build_only.so" not in
            bootstrap.read_bytes(), "bootstrap binary contains FC-1 payload path")
    require(expected_payload.decode() in
            BOOTSTRAP_SOURCE.read_text(encoding="utf-8"),
            "bootstrap source path differs from binary contract")

    disasm = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(controller)], text=True
    ).lower()
    for value in ("0x7ee8d18", "0x38b77cc", "0x38b7840"):
        require(value in disasm, f"linked controller lacks constant {value}")
    controller_bytes = controller.read_bytes()
    for token in (
        b"I_ACCEPT_FC2_THREE_FRAME_DEFERRED_REGISTRATION_OBSERVE_ONLY_V1",
        b"FC2_DONE success=%u",
    ):
        require(token in controller_bytes,
                f"linked controller lacks protocol token: {token!r}")

    build = BUILD.read_text(encoding="utf-8")
    require('throw "FC-2 runner policy is missing"' in build,
            "build can skip missing runner policy")
    offline = runner.index('if ($Mode -eq "OfflineValidateOnly")')
    adb = runner.index('if (-not (Test-Path -LiteralPath $AdbPath')
    require(offline < adb, "runner default can reach ADB")
    confirmed = runner.index("$confirmedFresh = $true")
    final = runner.index("} finally {", confirmed)
    force_stop = runner.index("Stop-ConfirmedFreshProcess $gamePid $startTime",
                              final)
    require(confirmed < final < force_stop,
            "confirmed probe failure is not force-stopped")
    require("if ($current -gt 0)" not in runner[
        runner.index("function Stop-ConfirmedFreshProcess"):offline
    ], "force-stop is conditional on a momentary PID")

    print("FC2_CROSS_ARTIFACT_AUDIT passed=1 pins=6 payload_arch=arm64 "
          "host_arch=x86_64 controller_imports=bounded bootstrap_path=fc2 "
          "failure_force_stop=unconditional device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
