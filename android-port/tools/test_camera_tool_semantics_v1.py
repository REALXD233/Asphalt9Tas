from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parent / "source" / "AluTasV2-main" / "AsphaltTool"


def apply_absolute(natural: tuple[float, ...], flags: int,
                   position: tuple[float, float, float],
                   rotation: tuple[float, float, float, float],
                   fov: float) -> tuple[tuple[float, ...], float]:
    transform = list(natural[:7])
    natural_fov = natural[7]
    if flags & 1:
        transform[:3] = position
    if flags & 2:
        transform[3:] = rotation
    return tuple(transform), fov if flags & 4 else natural_fov


class CameraToolSemanticsTest(unittest.TestCase):
    def test_upstream_modes_and_flags_are_preserved(self) -> None:
        layer = (UPSTREAM / "tool" / "src" / "layer" /
                 "CameraToolLayer.cpp").read_text(encoding="utf-8")
        header = (UPSTREAM / "tool" / "src" / "layer" /
                  "CameraToolLayer.h").read_text(encoding="utf-8")
        shared = (UPSTREAM / "shared" / "src" /
                  "Communication.h").read_text(encoding="utf-8")
        self.assertIn("FREE_CAM, ORBITAL_CAM, FRONT_CAR", header)
        self.assertIn("CONTINUOUS_OVERRIDE_POSITION", layer)
        self.assertIn("CONTINUOUS_OVERRIDE_ROTATION", layer)
        self.assertIn("CONTINUOUS_OVERRIDE_FOV_RAD", layer)
        self.assertIn("CONTINUOUS_OVERRIDE_RELATIVE_TO_CAR", layer)
        self.assertIn("CONTINUOUS_OVERRIDE_POSITION           = 1 << 0", shared)
        self.assertIn("CONTINUOUS_OVERRIDE_ROTATION           = 1 << 1", shared)
        self.assertIn("CONTINUOUS_OVERRIDE_FOV_RAD            = 1 << 2", shared)
        self.assertIn("CONTINUOUS_OVERRIDE_RELATIVE_TO_CAR    = 1 << 3", shared)

    def test_upstream_game_update_is_original_first(self) -> None:
        detour = (UPSTREAM / "dll" / "src" /
                  "DetourFunctions.cpp").read_text(encoding="utf-8")
        start = detour.index("void REROUTE_FUNCTION Detour_CameraUpdate")
        body = detour[start:detour.index("void PatchDisableGameFovWriteInstruction", start)]
        self.assertLess(body.index("RealCameraUpdateCall(rcx)"),
                        body.index("CONTINUOUS_OVERRIDE_POSITION"))
        self.assertLess(body.index("RealCameraUpdateCall(rcx)"),
                        body.index("CONTINUOUS_OVERRIDE_ROTATION"))
        self.assertLess(body.index("RealCameraUpdateCall(rcx)"),
                        body.index("CONTINUOUS_OVERRIDE_FOV_RAD"))

    def test_independent_absolute_flags_preserve_natural_components(self) -> None:
        natural = (1, 2, 3, 0, 0, 0, 1, 0.9)
        position = (10, 20, 30)
        rotation = (0.1, 0.2, 0.3, 0.9)
        transform, fov = apply_absolute(natural, 1, position, rotation, 1.2)
        self.assertEqual(transform, (10, 20, 30, 0, 0, 0, 1))
        self.assertEqual(fov, 0.9)
        transform, fov = apply_absolute(natural, 2 | 4, position, rotation, 1.2)
        self.assertEqual(transform, (1, 2, 3, 0.1, 0.2, 0.3, 0.9))
        self.assertEqual(fov, 1.2)

    def test_camera_tool_is_not_a_replay_camera_track(self) -> None:
        replay = (UPSTREAM / "tool" / "src" / "common" /
                  "Replay.cpp").read_text(encoding="utf-8")
        self.assertNotIn("RecordedCameraState", replay)
        self.assertNotIn("m_recorded_camera_state", replay)


if __name__ == "__main__":
    unittest.main()
