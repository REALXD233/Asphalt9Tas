#!/usr/bin/env python3
"""Source parity policy for the phase-aligned Camera Tool v2 core."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = ROOT / "src" / "camera_tool_runtime_core_v2.h"
PROTOCOL = ROOT / "src" / "camera_tool_runtime_protocol_v2.h"


def main() -> int:
    text = CORE.read_text(encoding="utf-8") + PROTOCOL.read_text(encoding="utf-8")
    for needle in (
        "kFreeFlight = 2", "kOrbital = 3", "kKnownInputMask",
        "std::clamp(5.0f*state->move_speed, 1.0f, 200.0f)*dt",
        "state->move_speed = std::clamp(state->move_speed, 0.1f, 1000.0f)",
        "Scale(Normalize(world), state->move_speed*dt)",
        "ControllerRotation(input.yaw_degrees, input.pitch_degrees)",
        "std::clamp(pitch, -89.0f, 89.0f)",
        "Multiply(q_pitch, q_yaw)",
        "1.0f + state->orbital_distance*0.005f",
        "std::max(state->orbital_distance, 0.1f)",
        "Sub(target, Scale(forward, state->orbital_distance))",
        "return {value.x, value.z, -value.y}",
        "return {value.x, -value.z, value.y}",
        "constructed at 55 degrees",
        "std::clamp(input.dt_seconds, 0.0f, 0.05f)",
    ):
        assert needle in text, needle
    for forbidden in ("camera smoothing", "lerp", "slerp", "CameraReplay", "ptrace", "pwrite"):
        assert forbidden not in text, forbidden
    print(
        "CAMERA_TOOL_RUNTIME_CORE_V2_POLICY passed=1 phase_aligned=1 "
        "free=1 orbital=1 input_state=1 interpolation=0 replay_track=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
