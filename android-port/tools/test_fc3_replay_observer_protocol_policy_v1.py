#!/usr/bin/env python3
"""Offline policy and behavioral checks for the FC-3 observer protocol."""

from __future__ import annotations

import pathlib
import re
import struct
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fc3_replay_observer_protocol_v1.h"
SOURCE = ROOT / "src" / "fc3_replay_observer_state_machine_v1.cpp"
BUILD = ROOT / "build-fc3-replay-observer-protocol-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_source_policy() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    required_header = (
        "kRequiredFlags = 0x3FFFFu",
        "kWaitingDedicatedHit",
        "kWaitingCallbackClose",
        "kWaitingNextAccumulator",
        "kWaitingNextCallbackOpen",
        "kRejectActionQueueWrite",
        "kRejectNitroMutation",
        "kDedicatedMembershipAbsent",
        "phase_witness_accumulator",
        "test-only preconditions",
        "must submit recorded activations through the real game scheduler",
        "only a phase witness",
        "static_assert(sizeof(Snapshot) == 64",
        "static_assert(sizeof(Report) == 512",
        "class StateMachine",
    )
    for needle in required_header:
        require(needle in header, f"missing FC-3 protocol token: {needle}")
    require("next_step_accumulator" not in header,
            "FC-3 phase witness must not be named as the replay delta target")

    required_source = (
        "event.kind == EventKind::kActionQueueWrite",
        "event.kind == EventKind::kNitroActiveWrite",
        "event.kind == EventKind::kNitroModeWrite",
        "event.snapshot.action_queue_count != 0u",
        "event.snapshot.direct_mode != 0u",
        "event.snapshot.nitro_active != 0u",
        "PositiveFiniteAccumulator",
        "event.dedicated_members_full != 0u",
        "FC3_PROTOCOL_BUILD_ONLY runtime=disabled return=-100",
        "device_access=0 game_writes=0 action_calls=0 nitro_writes=0",
    )
    for needle in required_source:
        require(needle in source, f"missing FC-3 state-machine token: {needle}")

    forbidden = (
        "ptrace(", "pwrite(", "process_vm_writev", "PTRACE_POKEDATA",
        "PTRACE_SETREGS", "/proc/", "adb ", "input keyevent", "input tap",
        "CarPhysicsState_dispatch_action", "NitroState_handle_activation",
        "socket(", "pthread_create", "std::thread", "dlopen(", "mprotect(",
    )
    combined = header + source
    for needle in forbidden:
        require(needle not in combined,
                f"forbidden FC-3 protocol primitive: {needle}")

    order = [
        source.index("case Phase::kWaitingDedicatedHit"),
        source.index("case Phase::kWaitingCallbackClose"),
        source.index("case Phase::kWaitingNextAccumulator"),
        source.index("case Phase::kWaitingNextCallbackOpen"),
    ]
    require(order == sorted(order), "FC-3 phase source ordering")


class Model:
    WAIT_DEDICATED = 0
    WAIT_CLOSE = 1
    WAIT_ACCUMULATOR = 2
    WAIT_NEXT_OPEN = 3
    PROVED = 4
    REJECTED = 5

    def __init__(self, owner: int = 77) -> None:
        self.owner = owner
        self.phase = self.WAIT_DEDICATED
        self.anchor: dict[str, int] | None = None
        self.reason = ""

    def reject(self, reason: str) -> bool:
        self.phase = self.REJECTED
        self.reason = reason
        return False

    def consume(self, kind: str, snapshot: dict[str, int], members: int = 1,
                payload_failures: int = 0) -> bool:
        if self.phase in (self.PROVED, self.REJECTED):
            return False
        if snapshot["tid"] != self.owner:
            return self.reject("owner")
        if not (snapshot["begin"] <= snapshot["end"] <= snapshot["capacity"]):
            return self.reject("queue_shape")
        if snapshot["direct"] != 0:
            return self.reject("direct_mode")
        if payload_failures:
            return self.reject("payload")
        if kind == "queue_write":
            return self.reject("action")
        if kind in ("nitro_active_write", "nitro_mode_write"):
            return self.reject("nitro")
        if self.phase == self.WAIT_DEDICATED:
            if kind != "dedicated" or snapshot["flags"] != 1:
                return self.reject("order")
            if snapshot["direct"] != 0 or snapshot["count"] != 0:
                return self.reject("action_precondition")
            if snapshot["active"] != 0 or members != 1:
                return self.reject("dedicated_precondition")
            self.anchor = dict(snapshot)
            self.phase = self.WAIT_CLOSE
            return True
        assert self.anchor is not None
        for field in ("begin", "end", "capacity", "count"):
            if snapshot[field] != self.anchor[field]:
                return self.reject("action")
        for field in ("active", "mode"):
            if snapshot[field] != self.anchor[field]:
                return self.reject("nitro")
        if self.phase == self.WAIT_CLOSE:
            if kind != "close" or snapshot["flags"] != 0:
                return self.reject("order")
            self.phase = self.WAIT_ACCUMULATOR
            return True
        if self.phase == self.WAIT_ACCUMULATOR:
            value = struct.unpack("<f", struct.pack("<I", snapshot["acc"]))[0]
            if kind != "accumulator" or snapshot["flags"] != 0 or not (0 < value <= 1):
                return self.reject("accumulator")
            self.phase = self.WAIT_NEXT_OPEN
            return True
        if self.phase == self.WAIT_NEXT_OPEN:
            if kind != "next_open" or snapshot["flags"] != 1 or members != 0:
                return self.reject("cleanup")
            self.phase = self.PROVED
            return True
        return self.reject("state")


def snapshot(flags: int = 1) -> dict[str, int]:
    return {
        "tid": 77, "flags": flags, "direct": 0, "active": 0, "mode": 0,
        "begin": 0x1000, "end": 0x1000, "capacity": 0x1040, "count": 0,
        "acc": struct.unpack("<I", struct.pack("<f", 1 / 60))[0],
    }


def verify_behavioral_model() -> None:
    model = Model()
    require(model.consume("dedicated", snapshot(1)), "dedicated")
    require(model.consume("close", snapshot(0)), "close")
    require(model.consume("accumulator", snapshot(0)), "accumulator")
    require(model.consume("next_open", snapshot(1), members=0), "next open")
    require(model.phase == Model.PROVED, "success lifecycle")

    negative_cases: list[tuple[str, str]] = []
    for name, mutate in (
        ("wrong_owner", lambda s: s.__setitem__("tid", 78)),
        ("direct_mode", lambda s: s.__setitem__("direct", 1)),
        ("queue_nonempty", lambda s: (s.__setitem__("end", 0x1008),
                                      s.__setitem__("count", 1))),
        ("nitro_active", lambda s: s.__setitem__("active", 1)),
    ):
        test = Model()
        first = snapshot(1)
        mutate(first)
        require(not test.consume("dedicated", first), name)
        negative_cases.append((name, test.reason))

    for kind in ("queue_write", "nitro_active_write", "nitro_mode_write"):
        test = Model()
        require(test.consume("dedicated", snapshot(1)), f"{kind} setup")
        require(not test.consume(kind, snapshot(1)), f"reject {kind}")
        negative_cases.append((kind, test.reason))

    test = Model()
    require(test.consume("dedicated", snapshot(1)), "order setup")
    require(not test.consume("accumulator", snapshot(0)), "reject skipped close")
    negative_cases.append(("skipped_close", test.reason))

    test = Model()
    require(test.consume("dedicated", snapshot(1)), "cleanup setup 1")
    require(test.consume("close", snapshot(0)), "cleanup setup 2")
    require(test.consume("accumulator", snapshot(0)), "cleanup setup 3")
    require(not test.consume("next_open", snapshot(1), members=1),
            "reject retained callback")
    negative_cases.append(("retained_callback", test.reason))

    test = Model()
    require(test.consume("dedicated", snapshot(1)), "direct setup")
    changed = snapshot(0)
    changed["direct"] = 1
    require(not test.consume("close", changed), "reject post-gate direct mode")
    negative_cases.append(("post_gate_direct_mode", test.reason))

    require(len(negative_cases) == 10, "negative-case coverage")


def verify_artifacts(passive: pathlib.Path, selftest_object: pathlib.Path,
                     readelf: pathlib.Path, objdump: pathlib.Path) -> None:
    for path in (passive, selftest_object, readelf, objdump):
        require(path.is_file(), f"missing FC-3 artifact/tool: {path}")
    passive_header = subprocess.check_output(
        [str(readelf), "-h", str(passive)], text=True
    )
    object_header = subprocess.check_output(
        [str(readelf), "-h", str(selftest_object)], text=True
    )
    require("Advanced Micro Devices X86-64" in passive_header,
            "FC-3 passive architecture")
    require("Advanced Micro Devices X86-64" in object_header and
            re.search(r"Type:\s+REL", object_header),
            "FC-3 selftest review object architecture/type")
    passive_symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(passive)], text=True
    )
    for name in ("ptrace", "pwrite", "process_vm_writev", "open", "waitpid",
                 "socket", "dlopen", "mprotect"):
        require(re.search(rf"\b{re.escape(name)}(?:@|\b)", passive_symbols) is None,
                f"passive FC-3 imports forbidden primitive: {name}")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(selftest_object)], text=True
    )
    require("StateMachine::Consume" in disassembly,
            "FC-3 state machine missing from review object")


def main() -> int:
    verify_source_policy()
    verify_behavioral_model()
    if len(sys.argv) == 5:
        verify_artifacts(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(
            f"usage: {sys.argv[0]} [passive selftest-object readelf objdump]"
        )
    print("FC3_PROTOCOL_POLICY passed=1 sequence=dedicated-close-accumulator-open "
          "negative_cases=10 observe_only=1 device_access=0 game_writes=0 "
          "action_calls=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
