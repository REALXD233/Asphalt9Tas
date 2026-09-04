#!/usr/bin/env python3
"""Read-only A9 artifact inspector (B4/N1).

Strictly offline, strictly read-only.  The inspector never writes, converts,
repairs or rewrites the input file.  It identifies a supported A9 binary
format from its 8-byte magic and version, then delegates the full structural
validation to the authoritative decoder/validator modules already checked in
under ``android-port/tools/``.  No format layout is re-guessed here; every
``SUPPORTED`` format runs the same strict checks its own decoder enforces.

Exit codes (see handoff section 3.4):
  0  supported format, all strict checks passed
  1  recognized supported format but the file is corrupt/truncated/semantically
     invalid
  2  unknown magic, unsupported/old version, missing FC-1 pid/base, or a
     format that is known but not supported by this inspector
  3  file missing/unreadable or CLI usage error
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

# Authoritative decoders/validators (read-only imports; nothing here modifies
# them or the input files).
import native_physics_recording_v1 as a9nps1_codec
import unified_tick_recording_v1 as a9utk1_codec
import synchronized_tick_recording_v1 as a9usr1_codec
import synchronized_brake_recording_v1 as a9usr2_codec
import synchronized_action_window_recording_v1 as a9usr3_codec
import synchronized_action_until_release_recording_v1 as a9usr4_codec
import parse_unified_executor_report_v1 as a9uer1_codec
import parse_unified_executor_report_v2 as a9uer2_codec
import parse_unified_executor_report_v3 as a9uer3_codec
import parse_unified_executor_report_v4 as a9uer4_codec
import parse_unified_executor_report_v5 as a9uer5_codec
import parse_unified_executor_report_v6 as a9uer6_codec
import parse_unified_nitro_observe_report_v7 as a9uer7_codec
import validate_fc1_report_v1 as fc1_codec

EXIT_VALID = 0
EXIT_INVALID = 1
EXIT_UNSUPPORTED = 2
EXIT_IO_OR_USAGE = 3

MAGIC_SIZE = 8

FC1_VERSION = 1


def _u32(blob: bytes, offset: int = 8) -> int:
    return struct.unpack_from("<I", blob, offset)[0]


# ---------------------------------------------------------------------------
# Per-format adapters.  Each adapter only reads the authoritative decoder's
# result object; it never relaxes any decoder check.
# ---------------------------------------------------------------------------


def _decode_nps1(blob: bytes) -> dict[str, Any]:
    frames = a9nps1_codec.decode_recording(blob)
    a9nps1_codec.validate_runtime_safe(frames)
    return {
        "frames": len(frames),
        "first_tick": frames[0].tick,
        "last_tick": frames[-1].tick,
    }


def _decode_utk1(blob: bytes) -> dict[str, Any]:
    _, frames = a9utk1_codec.decode_recording(blob)
    return {
        "frames": len(frames),
        "first_tick": frames[0].tick,
        "last_tick": frames[-1].tick,
    }


def _decode_usr1(blob: bytes) -> dict[str, Any]:
    report = a9usr1_codec.decode_sync_report(blob)
    return {
        "frames": report.captured_frames,
        "first_tick": report.frames[0].tick,
        "last_tick": report.frames[-1].tick,
    }


def _usr2_input_cycle_profile(blob: bytes) -> bool:
    """Return whether A9USR2 uses the registered input-cycle anchor profile.

    The base and input-cycle reports share version 2 and the same physical
    layout.  Bit 9 is a witnessed semantic extension, not an unknown flag.
    Keep every other flag combination fail-closed.
    """
    if len(blob) < a9usr2_codec.HEADER_SIZE:
        raise ValueError("report is shorter than the A9USR2 header")
    flags = struct.unpack_from("<I", blob, 20)[0]
    base = a9usr2_codec.REQUIRED_FLAGS
    anchored = base | a9usr2_codec.INPUT_CYCLE_ANCHOR_FLAG
    if flags == base:
        return False
    if flags == anchored:
        return True
    raise ValueError(f"A9USR2 success flags are not a registered profile: 0x{flags:x}")


def _decode_usr2(blob: bytes) -> dict[str, Any]:
    anchored = _usr2_input_cycle_profile(blob)
    required_flags = a9usr2_codec.REQUIRED_FLAGS
    if anchored:
        required_flags |= a9usr2_codec.INPUT_CYCLE_ANCHOR_FLAG
    report = a9usr2_codec.decode_sync_brake_report(
        blob, required_flags=required_flags
    )
    return {
        "frames": report.captured_frames,
        "first_tick": report.frames[0].tick,
        "last_tick": report.frames[-1].tick,
    }


def _verify_usr2_companion(blob: bytes, companion_blob: bytes) -> Any:
    return a9usr2_codec.verify_synchronized_brake_capture(
        blob,
        companion_blob,
        require_input_cycle_anchor=_usr2_input_cycle_profile(blob),
    )


def _decode_usr3(blob: bytes) -> dict[str, Any]:
    report = a9usr3_codec.decode_action_window_report(blob)
    return {
        "frames": report.captured_frames,
        "first_tick": report.frames[0].tick,
        "last_tick": report.frames[-1].tick,
    }


def _decode_usr4(blob: bytes) -> dict[str, Any]:
    result = a9usr4_codec.decode_action_until_release_report(blob)
    return {
        "frames": result.report.captured_frames,
        "first_tick": result.report.frames[0].tick,
        "last_tick": result.report.frames[-1].tick,
    }


def _summary_view(summary: Any) -> dict[str, Any]:
    return {
        "frames": summary.frames,
        "first_tick": summary.first_tick,
        "last_tick": summary.last_tick,
    }


def _decode_uer1(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer1_codec.decode_report(blob))


def _decode_uer2(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer2_codec.decode_report(blob))


def _decode_uer3(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer3_codec.decode_report(blob))


def _decode_uer4(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer4_codec.decode_report(blob))


def _decode_uer5(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer5_codec.decode_report(blob))


def _decode_uer6(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer6_codec.decode_report(blob))


def _decode_uer7(blob: bytes) -> dict[str, Any]:
    return _summary_view(a9uer7_codec.decode_report(blob))


def _decode_fc1(blob: bytes, pid: int, base: int) -> dict[str, Any]:
    if len(blob) >= 12 and _u32(blob, 8) != FC1_VERSION:
        raise ValueError(
            f"unsupported FC-1 report version {_u32(blob, 8)} "
            f"(expected {FC1_VERSION})"
        )
    fc1_codec.validate(blob, pid, base)
    return {"frames": None, "first_tick": None, "last_tick": None}


COMMON_CHECKS = (
    "magic",
    "version",
    "abi_sizes",
    "length",
    "frame_count",
    "tick_sequence",
    "finite_floats",
    "flags",
    "build_id",
    "reserved",
)


@dataclass(frozen=True)
class FormatSpec:
    name: str
    magic: bytes | None
    version: int | None
    kind: str  # "supported" | "known_unsupported" | "requires_explicit_format"
    authoritative: str
    decoder: Callable[..., dict[str, Any]] | None = None
    needs_pid_base: bool = False
    checks: tuple[str, ...] = COMMON_CHECKS
    notes: str = ""
    companion_verifier: Callable[[bytes, bytes], Any] | None = None
    companion_format: str | None = None
    companion_authoritative: str = ""


# A9USR report formats that can be cross-bound to an explicitly provided
# A9UTK1 recording.  Companion files are NEVER auto-discovered.
COMPANION_FORMATS = frozenset({"A9USR1", "A9USR2", "A9USR3", "A9USR4"})


# Formats with strict authoritative decoders and validated synthetic fixtures
# (see test_inspect_a9_artifact_v1.py).
SUPPORTED_FORMATS: tuple[FormatSpec, ...] = (
    FormatSpec(
        "A9NPS1",
        a9nps1_codec.MAGIC,
        a9nps1_codec.VERSION,
        "supported",
        "tools/native_physics_recording_v1.py",
        _decode_nps1,
        checks=COMMON_CHECKS + ("monotonic_time",),
    ),
    FormatSpec(
        "A9UTK1",
        a9utk1_codec.MAGIC,
        a9utk1_codec.VERSION,
        "supported",
        "tools/unified_tick_recording_v1.py",
        _decode_utk1,
        checks=COMMON_CHECKS + ("fixed_interval", "controls"),
    ),
    FormatSpec(
        "A9USR1",
        a9usr1_codec.MAGIC,
        a9usr1_codec.VERSION,
        "supported",
        "tools/synchronized_tick_recording_v1.py",
        _decode_usr1,
        checks=COMMON_CHECKS + ("identity", "event_order", "error_counters", "controls"),
        companion_verifier=a9usr1_codec.verify_synchronized_capture,
        companion_format="A9UTK1",
        companion_authoritative="tools/synchronized_tick_recording_v1.py:verify_synchronized_capture",
    ),
    FormatSpec(
        "A9USR2",
        a9usr2_codec.MAGIC,
        a9usr2_codec.VERSION,
        "supported",
        "tools/synchronized_brake_recording_v1.py",
        _decode_usr2,
        checks=COMMON_CHECKS + ("identity", "event_order", "error_counters", "brake_pair"),
        companion_verifier=_verify_usr2_companion,
        companion_format="A9UTK1",
        companion_authoritative="tools/synchronized_brake_recording_v1.py:verify_synchronized_brake_capture",
    ),
    FormatSpec(
        "A9USR3",
        a9usr3_codec.MAGIC,
        a9usr3_codec.VERSION,
        "supported",
        "tools/synchronized_action_window_recording_v1.py",
        _decode_usr3,
        checks=COMMON_CHECKS + ("identity", "event_order", "error_counters", "brake_pair"),
        companion_verifier=a9usr3_codec.verify_action_window_capture,
        companion_format="A9UTK1",
        companion_authoritative="tools/synchronized_action_window_recording_v1.py:verify_action_window_capture",
    ),
    FormatSpec(
        "A9USR4",
        a9usr4_codec.MAGIC,
        a9usr4_codec.VERSION,
        "supported",
        "tools/synchronized_action_until_release_recording_v1.py",
        _decode_usr4,
        checks=COMMON_CHECKS + ("identity", "event_order", "error_counters", "brake_pair", "bounded_maximum"),
        companion_verifier=a9usr4_codec.verify_action_until_release_capture,
        companion_format="A9UTK1",
        companion_authoritative="tools/synchronized_action_until_release_recording_v1.py:verify_action_until_release_capture",
    ),
    FormatSpec(
        "A9UER1",
        a9uer1_codec.MAGIC,
        a9uer1_codec.VERSION,
        "supported",
        "tools/parse_unified_executor_report_v1.py",
        _decode_uer1,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters"),
    ),
    FormatSpec(
        "A9UER2",
        a9uer2_codec.MAGIC,
        a9uer2_codec.VERSION,
        "supported",
        "tools/parse_unified_executor_report_v2.py",
        _decode_uer2,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters"),
    ),
    FormatSpec(
        "A9UER3",
        a9uer3_codec.MAGIC,
        a9uer3_codec.VERSION,
        "supported",
        "tools/parse_unified_executor_report_v3.py",
        _decode_uer3,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters", "deferred_clear"),
    ),
    FormatSpec(
        "A9UER4",
        a9uer4_codec.MAGIC,
        a9uer4_codec.VERSION,
        "supported",
        "tools/parse_unified_executor_report_v4.py",
        _decode_uer4,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters", "commit_tid"),
    ),
    FormatSpec(
        "A9UER5",
        a9uer5_codec.MAGIC,
        a9uer5_codec.VERSION,
        "supported",
        "tools/parse_unified_executor_report_v5.py",
        _decode_uer5,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters", "steering_audit"),
    ),
    FormatSpec(
        "A9UER6",
        a9uer6_codec.MAGIC,
        a9uer6_codec.VERSION,
        "supported",
        "tools/parse_unified_executor_report_v6.py",
        _decode_uer6,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters", "brake_audit"),
    ),
    FormatSpec(
        "A9UER7",
        a9uer7_codec.MAGIC,
        a9uer7_codec.VERSION,
        "supported",
        "tools/parse_unified_nitro_observe_report_v7.py",
        _decode_uer7,
        checks=COMMON_CHECKS + ("identity", "event_order", "counters", "rpc_counters", "nitro_response"),
    ),
    FormatSpec(
        "FC1",
        None,
        FC1_VERSION,
        "requires_explicit_format",
        "tools/validate_fc1_report_v1.py",
        _decode_fc1,
        needs_pid_base=True,
        checks=("size", "header_magic", "version", "flags", "phase", "identity", "threads", "frame_boundary", "counters", "evidence"),
        notes="336-byte report; validated only with --format FC1 --pid <pid> --base <base>",
    ),
)

# Other A9 magics observed in the workspace registry.  They are known but are
# deliberately NOT advertised as supported this round: no strict decoder with
# a validated synthetic fixture is wired in here, so any match fails closed
# with exit code 2 instead of being guessed.
KNOWN_UNSUPPORTED_FORMATS: tuple[FormatSpec, ...] = (
    FormatSpec("A9CDT1", b"A9CDT1\0\0", 1, "known_unsupported", "tools/parse_conditional_audit_v1.py"),
    FormatSpec("A9BAV1", b"A9BAV1\0\0", 1, "known_unsupported", "tools/parse_hwbp_barrel_angular_v1.py"),
    FormatSpec("A9ESA1", b"A9ESA1\0\0", 1, "known_unsupported", "tools/parse_hwbp_executor_stack_affinity_v1.py"),
    FormatSpec("A9PIP1", b"A9PIP1\0\0", 1, "known_unsupported", "tools/parse_hwbp_pipeline_order_v1.py"),
    FormatSpec("A9WBP1", b"A9WBP1\0\0", 1, "known_unsupported", "tools/parse_hwbp_worker_boundary_v1.py"),
    FormatSpec("A9WSS1", b"A9WSS1\0\0", 1, "known_unsupported", "tools/parse_hwbp_worker_stack_scope_v1.py"),
    FormatSpec("A9ZGB1", b"A9ZGB1\0\0", 1, "known_unsupported", "tools/parse_hwbp_zero_gap_v1.py"),
    FormatSpec("A9NTA1", b"A9NTA1\0\0", 1, "known_unsupported", "tools/parse_nitro_thread_affinity_report_v1.py"),
    FormatSpec("A9PEA1", b"A9PEA1\0\0", 1, "known_unsupported", "tools/parse_physics_executor_affinity_v1.py"),
    FormatSpec("A9SBT1", b"A9SBT1\0\0", 1, "known_unsupported", "tools/parse_same_bytes_audit_v1.py"),
    FormatSpec("A9PST1", b"A9PST1\0\0", None, "known_unsupported", "tools/parse_vehicle_state_trace_v1.py"),
    FormatSpec("A9NPA1", b"A9NPA1\0\0", 1, "known_unsupported", "(anchor parser not wired this round)"),
    FormatSpec("A9SPR1", b"A9SPR1\0\0", 1, "known_unsupported", "(legacy probe format, not wired this round)"),
    FormatSpec("A9NRS1", b"A9NRS1\0\0", 1, "known_unsupported", "(embedded nitro response, not a standalone artifact)"),
)

ALL_FORMATS: tuple[FormatSpec, ...] = SUPPORTED_FORMATS + KNOWN_UNSUPPORTED_FORMATS
MAGIC_TO_SPEC: dict[bytes, FormatSpec] = {
    spec.magic: spec for spec in ALL_FORMATS if spec.magic is not None
}


def _identify(blob: bytes) -> tuple[FormatSpec | None, str | None]:
    """Return (spec, failure_reason).  Only the 8-byte magic is used here."""
    if len(blob) < MAGIC_SIZE:
        return None, "file is shorter than an 8-byte magic"
    magic = blob[:MAGIC_SIZE]
    spec = MAGIC_TO_SPEC.get(magic)
    if spec is None:
        return None, "unknown magic"
    if spec.kind == "known_unsupported":
        return spec, f"format {spec.name} is known but not supported by this inspector"
    if spec.version is not None and len(blob) >= 12:
        version = _u32(blob)
        if version != spec.version:
            return spec, (
                f"unsupported version {version} (expected {spec.version} for {spec.name})"
            )
    return spec, None


def inspect_bytes(
    blob: bytes,
    *,
    format_name: str | None = None,
    pid: int | None = None,
    base: int | None = None,
    companion_blob: bytes | None = None,
) -> dict[str, Any]:
    """Validate a blob (and optionally an explicit A9UTK1 companion).

    The companion is only honored when the main file auto-identifies as
    A9USR1/A9USR2/A9USR3/A9USR4.  Companion files are never auto-discovered;
    the caller passes the bytes of an explicitly provided path.
    """
    size = len(blob)
    error: str | None = None
    status = "VALID"
    spec: FormatSpec | None = None
    summary: dict[str, Any] = {"frames": None, "first_tick": None, "last_tick": None}

    if format_name is not None:
        if format_name != "FC1":
            status = "UNSUPPORTED"
            error = f"unsupported --format value {format_name!r} (only FC1 requires explicit format)"
            return _report(status, None, None, size, summary, error)
        spec = next(item for item in SUPPORTED_FORMATS if item.name == "FC1")
        if companion_blob is not None:
            status = "USAGE_ERROR"
            error = "--companion cannot be combined with --format FC1"
            return _report(status, spec, None, size, summary, error)
        if pid is None or base is None:
            status = "UNSUPPORTED"
            error = "FC1 requires --pid and --base"
            return _report(status, spec, None, size, summary, error)
        try:
            summary = spec.decoder(blob, pid, base)  # type: ignore[misc]
        except (ValueError, struct.error) as exc:
            status = "INVALID"
            error = str(exc)
        return _report(status, spec, FC1_VERSION, size, summary, error)

    spec, failure = _identify(blob)
    version = _u32(blob) if len(blob) >= 12 else None

    # A companion is only meaningful for A9USR1-A9USR4 main files.
    if companion_blob is not None and (
        spec is None or spec.name not in COMPANION_FORMATS
    ):
        status = "USAGE_ERROR"
        error = "companion is only supported for A9USR1-A9USR4 main files"
        return _report(
            status, spec, version, size, summary, error,
            companion_format="A9UTK1",
        )

    if spec is None:
        return _report("UNSUPPORTED", None, None, size, summary, failure)
    if failure is not None:
        # Unsupported version: a provided companion was never evaluated.
        return _report(
            "UNSUPPORTED", spec, None, size, summary, failure,
            companion_status=(
                "NOT_EVALUATED" if companion_blob is not None else
                "NOT_PROVIDED" if spec.name in COMPANION_FORMATS else
                "NOT_APPLICABLE"
            ),
            companion_format=spec.companion_format,
        )

    # Main-file structural validation always runs first.  A structurally
    # invalid main file must never be disguised as a companion mismatch.
    try:
        summary = spec.decoder(blob)  # type: ignore[misc]
    except (ValueError, struct.error) as exc:
        status = "INVALID"
        error = f"main report: {exc}"
        return _report(
            status, spec, version, size, summary, error,
            companion_status=(
                "NOT_EVALUATED" if companion_blob is not None else
                "NOT_PROVIDED" if spec.name in COMPANION_FORMATS else
                "NOT_APPLICABLE"
            ),
            companion_format=spec.companion_format,
        )

    if companion_blob is not None:
        assert spec.companion_verifier is not None
        try:
            spec.companion_verifier(blob, companion_blob)
        except (ValueError, struct.error, StopIteration) as exc:
            status = "INVALID"
            error = f"companion/cross-bind: {exc}"
            return _report(
                status, spec, version, size, summary, error,
                validation_scope="CAPTURE_CROSS_BOUND",
                companion_status="INVALID",
                companion_format=spec.companion_format,
                capture_validated=False,
            )
        return _report(
            "VALID", spec, version, size, summary, None,
            validation_scope="CAPTURE_CROSS_BOUND",
            companion_status="VALID",
            companion_format=spec.companion_format,
            capture_validated=True,
            checks_extra=("companion_cross_bind",),
        )

    return _report(
        status, spec, version, size, summary, error,
        companion_status=(
            "NOT_PROVIDED" if spec.name in COMPANION_FORMATS else "NOT_APPLICABLE"
        ),
        companion_format=spec.companion_format,
    )


def _report(
    status: str,
    spec: FormatSpec | None,
    version: int | None,
    size: int,
    summary: dict[str, Any],
    error: str | None,
    *,
    validation_scope: str = "STRUCTURE_ONLY",
    companion_status: str = "NOT_APPLICABLE",
    companion_format: str | None = None,
    capture_validated: bool = False,
    checks_extra: tuple[str, ...] = (),
) -> dict[str, Any]:
    checks: list[str] = []
    if spec is not None and status == "VALID":
        checks = list(spec.checks) + list(checks_extra)
    return {
        "status": status,
        "format": spec.name if spec is not None else None,
        "version": version,
        "size": size,
        "frames": summary.get("frames"),
        "first_tick": summary.get("first_tick"),
        "last_tick": summary.get("last_tick"),
        "checks": checks,
        "validation_scope": validation_scope,
        "companion_status": companion_status,
        "companion_format": companion_format,
        "capture_validated": capture_validated,
        "read_only": True,
        "device_access": 0,
        "error": error,
    }


def _exit_code(report: dict[str, Any]) -> int:
    status = report["status"]
    if status == "VALID":
        return EXIT_VALID
    if status == "INVALID":
        return EXIT_INVALID
    if status == "UNSUPPORTED":
        return EXIT_UNSUPPORTED
    return EXIT_IO_OR_USAGE  # IO_ERROR and USAGE_ERROR


def _render_text(report: dict[str, Any]) -> str:
    scope = report["validation_scope"]
    companion_status = report["companion_status"]
    capture_validated = "true" if report["capture_validated"] else "false"
    if report["status"] == "VALID":
        fmt = report["format"]
        version = report["version"] if report["version"] is not None else "na"
        frames = report["frames"] if report["frames"] is not None else "na"
        first = report["first_tick"] if report["first_tick"] is not None else "na"
        last = report["last_tick"] if report["last_tick"] is not None else "na"
        companion_format = report["companion_format"] or "na"
        return (
            f"A9_ARTIFACT_VALID format={fmt} version={version} size={report['size']} "
            f"frames={frames} first_tick={first} last_tick={last} "
            f"validation_scope={scope} companion_format={companion_format} "
            f"companion_status={companion_status} capture_validated={capture_validated} "
            f"read_only=1 device_access=0"
        )
    return (
        f"A9_ARTIFACT_INVALID status={report['status']} "
        f"reason={report['error'] or 'unknown'} validation_scope={scope} "
        f"companion_status={companion_status} capture_validated={capture_validated} "
        f"read_only=1 device_access=0"
    )


def _list_formats_text() -> str:
    lines = ["A9_ARTIFACT_FORMATS"]
    for spec in ALL_FORMATS:
        version = spec.version if spec.version is not None else "na"
        lines.append(
            f"{spec.name} {spec.kind} version={version} decoder={spec.authoritative}"
        )
    lines.append("read_only=1 device_access=0")
    return "\n".join(lines)


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:  # pragma: no cover - argparse path
        self.print_usage(sys.stderr)
        print(f"a9_artifact_error: {message}", file=sys.stderr)
        raise SystemExit(EXIT_IO_OR_USAGE)


def _escape_magic(magic: bytes) -> str:
    """Lossless escaped-ASCII representation (``A9NPS1\\0\\0``)."""
    out: list[str] = []
    for byte in magic:
        if byte == 0:
            out.append("\\0")
        elif 32 <= byte < 127:
            out.append(chr(byte))
        else:
            out.append(f"\\x{byte:02x}")
    return "".join(out)


def _list_formats_json() -> dict[str, Any]:
    formats: list[dict[str, Any]] = []
    for spec in ALL_FORMATS:
        # KNOWN_UNSUPPORTED formats execute no checks at all, so their check
        # list must be empty (FormatSpec carries a shared default otherwise).
        checks = (
            [] if spec.kind == "known_unsupported" else list(spec.checks)
        )
        formats.append({
            "name": spec.name,
            "kind": spec.kind,
            "magic_ascii": _escape_magic(spec.magic) if spec.magic is not None else None,
            "magic_hex": spec.magic.hex() if spec.magic is not None else None,
            "version": spec.version,
            "authoritative": spec.authoritative,
            "needs_pid_base": spec.needs_pid_base,
            "checks": checks,
            "companion_format": spec.companion_format,
            "companion_authoritative": spec.companion_authoritative or None,
        })
    return {
        "schema": "A9_FORMAT_LIST_V1",
        "formats": formats,
        "summary": {"format_count": len(formats)},
        "read_only": True,
        "device_access": 0,
    }


def main(argv: list[str] | None = None) -> int:
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, help="artifact/report file")
    parser.add_argument("--json", action="store_true", help="emit JSON on stdout only")
    parser.add_argument("--format", default=None, help="explicit format (only FC1 is supported)")
    parser.add_argument("--pid", type=int, default=None)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=None)
    parser.add_argument("--companion", type=Path, default=None,
                        help="explicit A9UTK1 companion (A9USR1-A9USR4 only); never auto-discovered")
    parser.add_argument("--list-formats", action="store_true")
    args = parser.parse_args(argv)

    # --list-formats is a mutually exclusive mode: nothing else may be
    # silently ignored.  --json is supported and emits the machine-readable
    # A9_FORMAT_LIST_V1 schema.
    if args.list_formats:
        if (args.path is not None or args.format is not None
                or args.pid is not None or args.base is not None
                or args.companion is not None):
            parser.error(
                "--list-formats is mutually exclusive with path/--format/"
                "--pid/--base/--companion"
            )
        if args.json:
            print(json.dumps(_list_formats_json(), ensure_ascii=False, indent=2))
        else:
            print(_list_formats_text())
        return EXIT_VALID

    if args.path is None:
        parser.error("a path argument is required (or use --list-formats)")
    if args.format is None and (args.pid is not None or args.base is not None):
        parser.error("--pid/--base require --format FC1")
    if args.format is not None and args.format != "FC1":
        parser.error("only --format FC1 is supported")
    if args.companion is not None and args.format == "FC1":
        parser.error("--companion cannot be combined with --format FC1")

    empty_summary: dict[str, Any] = {
        "frames": None, "first_tick": None, "last_tick": None
    }

    try:
        blob = args.path.read_bytes()
    except OSError as error:
        report = _report(
            "IO_ERROR", None, None, 0, empty_summary,
            f"cannot read file: {error}",
        )
        if args.json:
            print(json.dumps(report, ensure_ascii=False, indent=2))
        else:
            print(_render_text(report))
        return EXIT_IO_OR_USAGE

    # Order matters: the main file is identified and structurally validated
    # BEFORE the explicit companion path is touched.  A main file that is not
    # an eligible A9USR1-A9USR4, or that fails its own validation, must never
    # trigger a companion I/O error or read the companion at all.
    report = inspect_bytes(
        blob, format_name=args.format, pid=args.pid, base=args.base
    )
    main_format = report["format"]
    main_version = report["version"]
    main_frames = report["frames"]
    main_first = report["first_tick"]
    main_last = report["last_tick"]

    if args.companion is not None:
        if report["status"] == "VALID" and main_format in COMPANION_FORMATS:
            try:
                companion_blob = args.companion.read_bytes()
            except OSError as error:
                report = _report(
                    "IO_ERROR", None, None, len(blob), empty_summary,
                    f"cannot read companion file: {error}",
                    companion_format="A9UTK1",
                    companion_status="IO_ERROR",
                )
                # Keep the validated main-file identity fields.
                report["format"] = main_format
                report["version"] = main_version
                report["frames"] = main_frames
                report["first_tick"] = main_first
                report["last_tick"] = main_last
                if args.json:
                    print(json.dumps(report, ensure_ascii=False, indent=2))
                else:
                    print(_render_text(report))
                return EXIT_IO_OR_USAGE
            report = inspect_bytes(
                blob, format_name=args.format, pid=args.pid, base=args.base,
                companion_blob=companion_blob,
            )
        elif report["status"] == "VALID":
            # Non-A9USR main file: usage error, companion never read.
            report = _report(
                "USAGE_ERROR", None, None, len(blob), empty_summary,
                "companion is only supported for A9USR1-A9USR4 main files",
                companion_format="A9UTK1",
            )
            report["format"] = main_format
            report["version"] = main_version
        else:
            # Main file failed or is unsupported.  Only A9USR1-A9USR4 main
            # files keep their own result with a NOT_EVALUATED companion;
            # any other main file (unknown magic, KNOWN_UNSUPPORTED or a
            # supported non-A9USR format) is a companion usage error and the
            # companion is never read.
            if main_format not in COMPANION_FORMATS:
                report = _report(
                    "USAGE_ERROR", None, None, len(blob), empty_summary,
                    "companion is only supported for A9USR1-A9USR4 main files",
                    companion_format="A9UTK1",
                )
                report["format"] = main_format
                report["version"] = main_version
            else:
                report["companion_status"] = "NOT_EVALUATED"
                report["companion_format"] = "A9UTK1"
                report["capture_validated"] = False

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(_render_text(report))
    return _exit_code(report)


if __name__ == "__main__":
    raise SystemExit(main())
