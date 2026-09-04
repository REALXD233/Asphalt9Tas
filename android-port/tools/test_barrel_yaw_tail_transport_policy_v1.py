#!/usr/bin/env python3
"""Audit the transient publish-last BarrelYaw transport offline."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
HEADER = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_transport_v1.h"
SELFTEST = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_transport_selftest_v1.cpp"


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
    for path in (artifact, readelf, HEADER, SELFTEST):
        require(path.is_file(), f"missing required file: {path}")
    header = HEADER.read_text(encoding="utf-8")
    for token in (
        "payload.targets, targets",
        "payload.shadow, result.prepared.shadow.data()",
        "payload.control, &result.prepared.unpublished_control",
        "payload.control + offsetof(protocol::Control, flags)",
        "&published_flags, sizeof(published_flags)",
        "observed_vptr != result.original_vptr",
        "inline bool BeginFrame(",
        "frame_start_evidence",
        "ReadOriginalTableExact",
        "ReadShadowExact",
        "ReadNativeBindingExact",
        "StoppedOwnerMatches",
        "stopped_tid == session.prepared.published_control.expected_tid",
        "offsetof(protocol::Control, active_audit_index)",
        "offsetof(protocol::Control, active_frame_index)",
        "offsetof(protocol::Control, active_token)",
        "session->issued_arm_sequences + 1u",
        "protocol::ArmToken(session->generation, sequence)",
        "prepared.shadow_vptr",
        "AuditMatchesArmedFrame",
        "protocol::kAuditVptrRestored",
        "payload.audits",
        "ReconcileCompletedWindow",
        "host::EvidenceDeltaMatches",
        "ObserveSettledAtF64",
        "ObserveIdleAtF64",
        "CancelAbsentFrameAtF64",
        "host::FrameTerminal(control, evidence, expected)",
        "StaticControlMatches(*session, control, true)",
        "ReadKnownObjectVptr(io, session",
        "RestoreVptr(io, session)",
        "session->frame_window_active",
    ):
        require(token in header, f"missing transport contract: {token}")
    arm_begin = header.find("inline bool ArmFrame(")
    arm_end = header.find("inline bool AuditMatchesArmedFrame(")
    require(0 <= arm_begin < arm_end, "ArmFrame boundary missing")
    arm = header[arm_begin:arm_end]
    audit_publish = arm.find("active_audit_index)")
    frame_publish = arm.find("active_frame_index)")
    token_publish = arm.find("active_token)")
    shadow_match = re.search(
        r"Write\(io,\s*(?:session->object|object),\s*"
        r"&(?:session->|result\.)prepared\.shadow_vptr",
        arm,
    )
    shadow_publish = -1 if shadow_match is None else shadow_match.start()
    require(0 <= audit_publish < frame_publish < token_publish < shadow_publish,
            "arm publication is not indices -> token -> shadow-vptr")
    require(arm.find("observed_vptr != session->prepared.shadow_vptr",
                     shadow_publish) > shadow_publish,
            "shadow-vptr final publication lacks readback")
    install_begin = header.find("inline bool Install(")
    install_end = header.find("inline bool BeginFrame(")
    require(0 <= install_begin < install_end, "Install boundary missing")
    install = header[install_begin:install_end]
    require("&result.prepared.shadow_vptr" not in install,
            "Install still publishes a persistent shadow vptr")
    cancel_begin = header.find("inline bool CancelAbsentFrameAtF64(")
    cancel_end = header.find("inline bool ObserveIdleAtF64(")
    require(0 <= cancel_begin < cancel_end, "Cancel boundary missing")
    cancel = header[cancel_begin:cancel_end]
    require(cancel.find("RestoreVptr(io, session)") <
            cancel.find("offsetof(protocol::Control, active_token)"),
            "cancel disarms before restoring the original vptr")
    for token in ("PTRACE_", "process_vm_", "/proc/", "pread(", "pwrite(",
                  "kill(", "waitpid(", "RemoteCall"):
        require(token not in header, f"transport gained runtime primitive: {token}")
    elf = run(str(readelf), "-h", str(artifact))
    require("Machine:" in elf and "X86-64" in elf,
            "transport selftest is not Android x86_64 ELF")
    rodata = run(str(readelf), "-p", ".rodata", str(artifact))
    require("BARREL_YAW_TAIL_TRANSPORT_SELFTEST" in rodata,
            "transport selftest receipt missing")
    print(
        "BARREL_YAW_TAIL_TRANSPORT_POLICY passed=1 config_only_install=1 "
        "flags_only_publish=1 full_table_identity=1 native_binding=1 "
        "indices_token_shadow_order=1 shadow_publish_last=1 "
        "monotonic_arm_sequence=1 exact_audit_delta=1 "
        "same_frame_reconcile=1 settled_f64_receipt=1 explicit_idle_receipt=1 "
        "stopped_tid_binding=1 "
        "cancel_restore_before_disarm=1 f64_terminal=1 "
        "foreign_vptr_fail_closed=1 finish_original_unarmed=1 "
        "runtime=disabled device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_YAW_TAIL_TRANSPORT_POLICY passed=0 error={error}",
              file=sys.stderr)
        raise SystemExit(1)
