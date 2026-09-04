from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / "source" / "AluTasV2-main" / "AsphaltTool"


class UpstreamCameraPlaybackSemanticsTest(unittest.TestCase):
    def test_normal_replay_does_not_publish_camera_packets(self) -> None:
        manager = (UPSTREAM / "tool" / "src" / "globalstate" /
                   "ReplayStateManager.cpp").read_text(encoding="utf-8")
        self.assertIn("new_frame.m_recorded_camera_state", manager)
        self.assertIn("TryPush(frame_opt->m_replay_input)", manager)
        self.assertNotIn("m_write_camera_state", manager)

    def test_persisted_replay_has_no_camera_stream(self) -> None:
        replay = (UPSTREAM / "tool" / "src" / "common" /
                  "Replay.cpp").read_text(encoding="utf-8")
        self.assertIn("namespace ReplayInputs", replay)
        self.assertIn("namespace RacerStates", replay)
        self.assertNotIn("RecordedCameraState", replay)
        self.assertNotIn("m_recorded_camera_state", replay)

    def test_camera_override_belongs_to_camera_tool(self) -> None:
        camera_tool = (UPSTREAM / "tool" / "src" / "layer" /
                       "CameraToolLayer.cpp").read_text(encoding="utf-8")
        self.assertIn("m_write_camera_state", camera_tool)
        self.assertIn("CONTINUOUS_OVERRIDE_POSITION", camera_tool)
        self.assertIn("CONTINUOUS_OVERRIDE_ROTATION", camera_tool)
        self.assertIn("CONTINUOUS_OVERRIDE_FOV_RAD", camera_tool)


if __name__ == "__main__":
    unittest.main()
