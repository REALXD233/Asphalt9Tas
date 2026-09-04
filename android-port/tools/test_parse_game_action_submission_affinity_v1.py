#!/usr/bin/env python3

from __future__ import annotations

import unittest

from parse_game_action_submission_affinity_v1 import decode_transcript


def synthetic_transcript() -> str:
    return "\n".join(
        (
            "GAME_ACTION_SUBMISSION_AFFINITY_V1_ARMED pid=100 "
            "owner=0x300000 queue_end=0x301368 active=0x400188 "
            "mode=0x40018c delta=0x200150 initial_count=0 threads=10 "
            "duration_ms=15000 writes=debug-registers-only game_calls=0 "
            "input_writes=0",
            "GAME_ACTION_SUBMISSION_EVENT seq=0 ns=100 tid=41 name=Input "
            "flags=Q--- count=1 direct=0 token=0x500000 completion=0 "
            "completion_ok=1 active=0 mode=0 delta=16667 rip=0x7000 "
            "read_ok=1 regs_ok=1",
            "GAME_ACTION_SUBMISSION_EVENT seq=1 ns=110 tid=52 name=FrameThread 0 "
            "flags=-AM- count=1 direct=1 token=0x500000 completion=1 "
            "completion_ok=1 active=1 mode=2 delta=16667 rip=0x7100 "
            "read_ok=1 regs_ok=1",
            "GAME_ACTION_SUBMISSION_EVENT seq=2 ns=120 tid=61 name=Physics "
            "flags=---D count=1 direct=0 token=0x500000 completion=1 "
            "completion_ok=1 active=1 mode=2 delta=16667 rip=0x7200 "
            "read_ok=1 regs_ok=1",
            "GAME_ACTION_SUBMISSION_EVENT seq=3 ns=130 tid=52 name=FrameThread 0 "
            "flags=Q--- count=0 direct=1 token=0x0 completion=255 "
            "completion_ok=0 active=1 mode=2 delta=16667 rip=0x7300 "
            "read_ok=1 regs_ok=1",
            "GAME_ACTION_SUBMISSION_AFFINITY_V1_DONE events=4 queue_hits=2 "
            "appends=1 cleanups=1 active=1 mode=1 delta=1 read_errors=0 "
            "ptrace_errors=0 unexpected_stops=0 clean=1 game_calls=0 "
            "input_writes=0",
        )
    ) + "\n"


class SubmissionAffinityParserTests(unittest.TestCase):
    def test_accepts_complete_natural_lifecycle(self) -> None:
        result = decode_transcript(synthetic_transcript())
        self.assertEqual(result["append_tids"], (41,))
        self.assertEqual(result["cleanup_tids"], (52,))
        self.assertEqual(result["nitro_tids"], (52,))
        self.assertEqual(result["delta_tids"], (61,))
        self.assertTrue(result["nitro_after_first_append"])
        self.assertEqual(result["nitro_submission_delay_ns"], 10)
        self.assertEqual(result["completion_at_append"], (0,))

    def test_rejects_missing_cleanup(self) -> None:
        text = synthetic_transcript().replace(
            "flags=Q--- count=0 direct=1 token=0x0",
            "flags=Q--- count=1 direct=1 token=0x0",
            1,
        )
        with self.assertRaisesRegex(ValueError, "counters mismatch|lifecycle"):
            decode_transcript(text)

    def test_rejects_unreadable_appended_token(self) -> None:
        text = synthetic_transcript().replace(
            "token=0x500000 completion=0 completion_ok=1",
            "token=0x0 completion=255 completion_ok=0",
            1,
        )
        with self.assertRaisesRegex(ValueError, "token is unreadable"):
            decode_transcript(text)

    def test_rejects_no_natural_delta(self) -> None:
        text = synthetic_transcript().replace("delta=16667 rip=0x7200", "delta=0 rip=0x7200")
        with self.assertRaisesRegex(ValueError, "nonzero delta"):
            decode_transcript(text)

    def test_rejects_unclean_capture(self) -> None:
        text = synthetic_transcript().replace("clean=1 game_calls", "clean=0 game_calls")
        with self.assertRaisesRegex(ValueError, "detach cleanly"):
            decode_transcript(text)

    def test_rejects_nitro_transition_without_live_submission(self) -> None:
        text = synthetic_transcript().replace(
            "flags=-AM- count=1", "flags=-AM- count=0", 1
        )
        with self.assertRaisesRegex(ValueError, "live submission"):
            decode_transcript(text)


if __name__ == "__main__":
    unittest.main()
