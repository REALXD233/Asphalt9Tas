#!/usr/bin/env python3
"""Reference-model tests for the natural action callback mailbox."""

from __future__ import annotations

import struct
import unittest


COMMAND = struct.Struct("<8sIIQIIQIIIIQ")
assert COMMAND.size == 64
MAGIC = b"A9NAC1\0\0"
VERSION = 1
REPLAY_FRAME = 1
NITRO_ENABLED = 2
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def checksum(blob: bytes) -> int:
    value = FNV_OFFSET
    for byte in blob[:56]:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def make_command(sequence: int, session: int, tid: int, count: int,
                 enabled: bool = True) -> bytes:
    flags = REPLAY_FRAME | (NITRO_ENABLED if enabled else 0)
    blob = COMMAND.pack(MAGIC, VERSION, 64, sequence, session, sequence - 1,
                        0x10000000, tid, count, flags, 0, 0)
    return blob[:56] + struct.pack("<Q", checksum(blob))


def valid(blob: bytes) -> bool:
    (magic, version, size, sequence, session, _frame, owner, tid, count,
     flags, reserved, saved_checksum) = COMMAND.unpack(blob)
    return (
        magic == MAGIC and version == VERSION and size == 64 and
        sequence > 0 and session > 0 and owner > 0 and tid > 0 and
        count <= 2 and flags & REPLAY_FRAME and not flags & ~3 and
        (flags & NITRO_ENABLED or count == 0) and reserved == 0 and
        saved_checksum == checksum(blob)
    )


class MailboxModel:
    def __init__(self, session: int):
        self.session = session
        self.selector = 0
        self.slots = [bytes(64), bytes(64)]
        self.claimed = 0
        self.completed = 0
        self.last_claimed = 0
        self.active: tuple[int, int, int] | None = None
        self.calls = 0
        self.fault = False

    def publish(self, blob: bytes) -> str:
        fields = COMMAND.unpack(blob)
        sequence, session, frame = fields[3], fields[4], fields[5]
        if not valid(blob):
            return "invalid"
        if session != self.session:
            return "wrong_session"
        if self.claimed != self.completed:
            return "pending"
        if sequence != self.completed + 1 or frame != self.completed:
            return "sequence"
        slot = sequence & 1
        self.slots[slot] = blob
        self.selector = (sequence << 1) | slot
        return "published"

    def claim(self, tid: int) -> str:
        if self.fault:
            return "latched"
        sequence = self.selector >> 1
        if not sequence or sequence == self.last_claimed:
            return "idle"
        blob = bytes(self.slots[self.selector & 1])
        fields = COMMAND.unpack(blob)
        if not valid(blob) or fields[3] != sequence:
            self.fault = True
            return "invalid"
        if fields[4] != self.session or sequence != self.last_claimed + 1:
            self.fault = True
            return "sequence"
        if fields[7] != tid:
            self.fault = True
            return "wrong_tid"
        self.last_claimed = sequence
        self.claimed = sequence
        self.active = (sequence, fields[5], fields[8])
        return "claimed"

    def complete(self, calls: int, semantic_ok: bool = True) -> str:
        if self.active is None or calls != self.active[2]:
            self.fault = True
            return "mismatch"
        if not semantic_ok:
            self.fault = True
            self.active = None
            return "semantic_failure"
        self.completed = self.active[0]
        self.calls = calls
        self.active = None
        return "completed"


class NaturalActionCallbackMailboxTests(unittest.TestCase):
    def test_zero_one_two_submit_exactly_once(self):
        model = MailboxModel(7)
        for count in range(3):
            self.assertEqual(model.publish(
                make_command(count + 1, 7, 4100, count)), "published")
            self.assertEqual(model.claim(4100), "claimed")
            self.assertEqual(model.complete(count), "completed")
            self.assertEqual(model.calls, count)
            self.assertEqual(model.claim(4100), "idle")

    def test_skip_and_enabled_zero_remain_distinct(self):
        skipped = make_command(1, 8, 4200, 0, False)
        enabled = make_command(1, 8, 4200, 0, True)
        self.assertTrue(valid(skipped))
        self.assertTrue(valid(enabled))
        self.assertNotEqual(skipped, enabled)
        self.assertFalse(valid(make_command(1, 8, 4200, 1, False)))

    def test_wrong_thread_latches(self):
        model = MailboxModel(9)
        self.assertEqual(model.publish(make_command(1, 9, 4300, 1)),
                         "published")
        self.assertEqual(model.claim(4301), "wrong_tid")
        self.assertEqual(model.claim(4300), "latched")

    def test_pending_and_sequence_gaps_are_rejected(self):
        model = MailboxModel(10)
        self.assertEqual(model.publish(make_command(1, 10, 4400, 1)),
                         "published")
        self.assertEqual(model.claim(4400), "claimed")
        self.assertEqual(model.publish(make_command(2, 10, 4400, 0)),
                         "pending")
        self.assertEqual(model.complete(1), "completed")
        self.assertEqual(model.publish(make_command(3, 10, 4400, 0)),
                         "sequence")

    def test_checksum_and_completion_mismatch_fail(self):
        damaged = bytearray(make_command(1, 11, 4500, 2))
        damaged[32] ^= 8
        self.assertFalse(valid(bytes(damaged)))
        model = MailboxModel(11)
        self.assertEqual(model.publish(make_command(1, 11, 4500, 2)),
                         "published")
        self.assertEqual(model.claim(4500), "claimed")
        self.assertEqual(model.complete(1), "mismatch")


if __name__ == "__main__":
    unittest.main()
