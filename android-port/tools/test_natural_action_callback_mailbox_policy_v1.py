#!/usr/bin/env python3
"""Static safety/ABI policy for the natural action callback mailbox."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src" / "natural_action_callback_mailbox_v1.h").read_text(
    encoding="utf-8"
)
SELFTEST = (
    ROOT / "src" / "natural_action_callback_mailbox_selftest_v1.cpp"
).read_text(encoding="utf-8")
BUILD = (ROOT / "build-natural-action-callback-mailbox-v1.ps1").read_text(
    encoding="utf-8"
)


class NaturalActionCallbackMailboxPolicyTests(unittest.TestCase):
    def test_contract_has_no_process_or_game_execution_primitive(self):
        lowered = HEADER.lower()
        for forbidden in (
            "ptrace",
            "pwrite",
            "/proc/",
            "socket(",
            "carphysicsstate_dispatch_action_158",
            "0x367b414",
            "nitroservice",
        ):
            self.assertNotIn(forbidden, lowered)

    def test_abi_is_three_cache_lines_with_checksum_bound_command(self):
        for required in (
            'static_assert(sizeof(Command) == 64',
            'static_assert(offsetof(Command, checksum) == 56',
            'static_assert(offsetof(Mailbox, slots) == 64',
            'static_assert(sizeof(Mailbox) == 192',
            "command.checksum == Checksum(command)",
        ):
            self.assertIn(required, HEADER)

    def test_publication_is_inactive_slot_then_release_selector(self):
        copy = HEADER.index("std::memcpy(&mailbox->slots[slot]")
        fence = HEADER.index("std::atomic_thread_fence(std::memory_order_release)", copy)
        publish = HEADER.index("StoreRelease(&mailbox->published_selector", fence)
        self.assertLess(copy, fence)
        self.assertLess(fence, publish)

    def test_original_nitro_count_and_skip_distinction_are_pinned(self):
        self.assertIn("kMaximumNitroActivations = 2", HEADER)
        self.assertIn("kNitroOverrideEnabled", HEADER)
        self.assertIn("nitro_enabled || command.nitro_activations == 0", HEADER)
        self.assertIn("TestSkipIsDifferentFromEnabledZero", SELFTEST)
        self.assertIn("TestExactZeroOneTwoAndIdleCallbacks", SELFTEST)

    def test_wrong_thread_pending_frame_and_dual_arch_build_are_pinned(self):
        self.assertIn("kWrongProducerThread", HEADER)
        self.assertIn("kPreviousFramePending", HEADER)
        self.assertIn("TestFailClosedBoundaries", SELFTEST)
        self.assertIn("x86_64-linux-android24-clang++.cmd", BUILD)
        self.assertIn("aarch64-linux-android24-clang++.cmd", BUILD)
        self.assertIn('"-Wall", "-Wextra", "-Werror"', BUILD)


if __name__ == "__main__":
    unittest.main()
