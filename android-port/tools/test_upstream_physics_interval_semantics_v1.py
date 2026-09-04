#!/usr/bin/env python3
"""Pin AluTasV2's default Physics Interval replay semantics to source."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2] / "source" / "AluTasV2-main"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    communication = (ROOT / "AsphaltTool/shared/src/Communication.h").read_text(
        encoding="utf-8")
    detours = (ROOT / "AsphaltTool/dll/src/DetourFunctions.cpp").read_text(
        encoding="utf-8")
    manager = (ROOT / "AsphaltTool/tool/src/globalstate/ReplayStateManager.cpp").read_text(
        encoding="utf-8")

    require(communication.count(
        "m_apply_physics_interval_override          = false") >= 2,
        "metadata defaults must leave Physics Interval override disabled")

    start = detours.index("Detour_GetPhysicsInterval")
    end = detours.index("bool SetupHook() noexcept", start)
    body = detours[start:end]
    original = body.index("RealGetPhysicsIntervalCall(p_this, p_out)")
    conditional = body.index("m_apply_physics_interval_override")
    override = body.index("*p_out = GameDLLState::g_current_state.m_meta_data.m_physics_interval")
    capture = body.index("m_physics_interval = *p_out")
    require(original < conditional < override < capture,
            "getter must be original, optional override, then final capture")

    start = manager.index("void OnInitNewReplay()")
    end = manager.index("void ClearInputCommandBuffer", start)
    replay_init = manager[start:end]
    require("m_fixed_frame_interval_micros" in replay_init,
            "replay must apply recorded logic-frame interval")
    require("m_apply_physics_interval_override" not in replay_init and
            "m_physics_interval" not in replay_init,
            "normal replay must not force the independent Physics Interval")
    print("UPSTREAM_PHYSICS_INTERVAL_SEMANTICS passed=1 "
          "default_override=0 real_first=1 optional_override=1 "
          "capture_final=1 replay_forces_logic_delta_only=1")


if __name__ == "__main__":
    main()
