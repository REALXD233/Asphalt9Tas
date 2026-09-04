#!/usr/bin/env python3
"""Reference-model tests for the two-slot Nitro pre-physics mailbox ABI."""

from __future__ import annotations

import dataclasses
import struct
import unittest


COMMAND = struct.Struct("<8sIIQIIQIIIIQ")
assert COMMAND.size == 64
MAGIC = b"A9NPC1\0\0"
VERSION = 1
FLAG_REPLAY = 1
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def checksum(blob: bytes) -> int:
    value = FNV_OFFSET
    for byte in blob[:56]:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def command(sequence: int, session: int, frame: int, tid: int, count: int) -> bytes:
    values = (MAGIC, VERSION, 64, sequence, session, frame, 0x10000000,
              tid, count, FLAG_REPLAY, 0, 0)
    blob = COMMAND.pack(*values)
    return blob[:56] + struct.pack("<Q", checksum(blob))


@dataclasses.dataclass
class Model:
    session: int = 0
    armed: bool = False
    selector: int = 0
    slots: list[bytes] = dataclasses.field(
        default_factory=lambda: [bytes(64), bytes(64)])
    last: int = 0
    ready: tuple[int, int, int] | None = None
    completed: int = 0
    calls: int = 0
    fault: str | None = None

    def publish(self, blob: bytes, sequence: int) -> None:
        slot = sequence & 1
        self.slots[slot] = blob
        self.selector = (sequence << 1) | slot

    def stage(self) -> str:
        if not self.armed:
            return "passive"
        if self.fault:
            return "latched"
        if self.ready is not None:
            self.fault = "previous_pending"
            return self.fault
        if not self.selector:
            self.fault = "missing"
            return self.fault
        selected = self.selector
        slot = selected & 1
        blob = bytes(self.slots[slot])
        if selected != self.selector:
            self.fault = "unstable"
            return self.fault
        fields = COMMAND.unpack(blob)
        (magic, version, size, sequence, session, _frame, owner, tid, count,
         flags, reserved, saved_checksum) = fields
        if not (magic == MAGIC and version == VERSION and size == 64 and
                sequence == selected >> 1 and sequence and owner and tid and
                count <= 2 and flags == FLAG_REPLAY and not reserved and
                saved_checksum == checksum(blob)):
            self.fault = "invalid"
            return self.fault
        if session != self.session:
            self.fault = "wrong_session"
            return self.fault
        if sequence <= self.last:
            self.fault = "duplicate" if sequence == self.last else "regression"
            return self.fault
        self.last = sequence
        self.ready = (sequence, tid, count)
        return "staged"

    def execute(self, tid: int) -> str:
        if self.ready is None:
            return "idle"
        sequence, expected, count = self.ready
        self.ready = None
        self.completed = sequence
        if tid != expected:
            self.fault = "wrong_tid"
            self.calls = 0
            return self.fault
        self.calls = count
        return "completed"


class MailboxTests(unittest.TestCase):
    def test_passive_default(self):
        self.assertEqual(Model().stage(), "passive")

    def test_zero_one_two_are_exact(self):
        for count in range(3):
            model = Model(session=9, armed=True)
            model.publish(command(1, 9, 44, 7000, count), 1)
            self.assertEqual(model.stage(), "staged")
            self.assertEqual(model.execute(7000), "completed")
            self.assertEqual(model.calls, count)
            self.assertEqual(model.execute(7000), "idle")

    def test_torn_inactive_slot_is_not_published(self):
        model = Model(session=3, armed=True)
        model.publish(command(1, 3, 0, 10, 1), 1)
        model.slots[0] = command(2, 3, 1, 10, 2)[:17] + bytes(47)
        self.assertEqual(model.stage(), "staged")
        self.assertEqual(model.execute(10), "completed")
        self.assertEqual(model.calls, 1)

    def test_checksum_session_and_tid_fail_closed(self):
        model = Model(session=4, armed=True)
        damaged = bytearray(command(1, 4, 0, 11, 1))
        damaged[40] ^= 1
        model.publish(bytes(damaged), 1)
        self.assertEqual(model.stage(), "invalid")

        model = Model(session=4, armed=True)
        model.publish(command(1, 5, 0, 11, 1), 1)
        self.assertEqual(model.stage(), "wrong_session")

        model = Model(session=4, armed=True)
        model.publish(command(1, 4, 0, 11, 1), 1)
        self.assertEqual(model.stage(), "staged")
        self.assertEqual(model.execute(12), "wrong_tid")
        self.assertEqual(model.calls, 0)

    def test_missing_duplicate_and_pending_fail_closed(self):
        model = Model(session=1, armed=True)
        self.assertEqual(model.stage(), "missing")

        model = Model(session=1, armed=True)
        model.publish(command(1, 1, 0, 20, 0), 1)
        self.assertEqual(model.stage(), "staged")
        model.publish(command(2, 1, 1, 20, 0), 2)
        self.assertEqual(model.stage(), "previous_pending")

        model = Model(session=1, armed=True)
        model.publish(command(1, 1, 0, 20, 0), 1)
        self.assertEqual(model.stage(), "staged")
        self.assertEqual(model.execute(20), "completed")
        self.assertEqual(model.stage(), "duplicate")


if __name__ == "__main__":
    unittest.main()
