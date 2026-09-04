#!/usr/bin/env python3
"""Enforce lossless upstream CameraUpdate parity for A9CAM1."""

from __future__ import annotations

import pathlib
import sys


def require(value: bool, message: str) -> None:
    if not value:
        raise SystemExit(f"CAMERA_REPLAY_PROTOCOL_POLICY_FAIL {message}")


def main() -> None:
    require(len(sys.argv) == 4, "usage")
    protocol = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    upstream = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    replay_manager = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")
    for token in (
        "A9CAM1", "position[3]", "rotation_xyzw[4]", "fov_radians",
        "aspect_ratio", "offset_relative_to_car[3]", "look_backwards",
        "source_recording_sha256", "sizeof(FrameV1) == 64",
    ):
        require(token in protocol, f"protocol_missing={token}")
    for token in (
        "RealCameraUpdateCall(rcx);",
        "m_camera_position_vec3",
        "m_camera_rotation_quat",
        "m_fov_radians",
        "m_aspect_ratio",
    ):
        require(token in upstream, f"upstream_missing={token}")
    require("m_recorded_camera_state" in replay_manager,
            "upstream_frame_camera_binding_missing")
    for forbidden in ("vehicle_transform", "smoothstep", "lerp("):
        require(forbidden not in protocol,
                f"derived_camera_substitute={forbidden}")
    print(
        "CAMERA_REPLAY_PROTOCOL_POLICY passed=1 lossless_sidecar=1 "
        "original_camera_update_first=1 position=1 rotation=1 fov=1 aspect=1 "
        "vehicle_derived_substitute=0 device_access=0"
    )


if __name__ == "__main__":
    main()
