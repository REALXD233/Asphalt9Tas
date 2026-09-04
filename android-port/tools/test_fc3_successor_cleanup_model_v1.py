#!/usr/bin/env python3
"""Adversarial offline model for FC-3 successor rollback gating.

The model deliberately covers abnormal stop/freeze outcomes.  It never opens a
process, writes memory, invokes a runner, or talks to a device.
"""

from __future__ import annotations

import enum
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp"


class Vptr(enum.Enum):
    ORIGINAL = "original"
    SHADOW = "shadow"
    UNKNOWN = "unknown"


class Decision(enum.Enum):
    ALREADY_RESTORED = "already_restored"
    CONDITIONAL_ROLLBACK = "conditional_rollback"
    FORCE_STOP_FRESH_PROCESS = "force_stop_fresh_process"
    READ_FAILED = "read_failed"


class CleanupDisposition(enum.Enum):
    CLEAN = "clean"
    FORCE_STOP_FRESH_PROCESS = "force_stop_fresh_process"


def decide_cleanup(*, read_ok: bool, current: Vptr,
                   owner_stopped: bool, other_threads_stopped: bool,
                   all_live_threads_stopped: bool) -> Decision:
    """Mirror the successor's fail-closed cleanup decision boundary."""
    if not read_ok:
        return Decision.READ_FAILED
    if current is Vptr.ORIGINAL:
        return Decision.ALREADY_RESTORED
    complete_freeze = (owner_stopped and other_threads_stopped and
                       all_live_threads_stopped)
    if current is Vptr.SHADOW and complete_freeze:
        return Decision.CONDITIONAL_ROLLBACK
    return Decision.FORCE_STOP_FRESH_PROCESS


def final_barrier_accepts(*, before_tids: set[int], after_tids: set[int],
                          owner_dr6: int,
                          other_stops: list[tuple[bool, int, int]]) -> bool:
    """Model (stopped, ptrace_event, dr6) for each non-owner tracee."""
    return (before_tids == after_tids and owner_dr6 & 0xF == 0x1 and
             all(stopped and event == 128 and dr6 & 0xF == 0
                 for stopped, event, dr6 in other_stops))


def transition_cleanup(*, any_thread_running: bool, refreeze_ok: bool,
                       restore_detach_ok: bool, process_alive: bool,
                       tracer_clear: bool) -> CleanupDisposition:
    complete_freeze = not any_thread_running or refreeze_ok
    if (complete_freeze and restore_detach_ok and process_alive and
            tracer_clear):
        return CleanupDisposition.CLEAN
    return CleanupDisposition.FORCE_STOP_FRESH_PROCESS


def decode_dr7_slot(dr7: int, slot: int) -> tuple[bool, bool, int, int]:
    local_enabled = bool(dr7 & (1 << (slot * 2)))
    global_enabled = bool(dr7 & (1 << (slot * 2 + 1)))
    control = (dr7 >> (16 + slot * 4)) & 0xF
    rw = control & 0x3
    length_encoding = (control >> 2) & 0x3
    length = {0: 1, 1: 2, 2: 8, 3: 4}[length_encoding]
    return local_enabled, global_enabled, rw, length


def bounded_wait_accepts(outcomes: list[str], deadline_polls: int) -> bool:
    """Model WNOHANG/EINTR polling under one monotonic deadline."""
    for poll in range(deadline_polls):
        outcome = outcomes[poll] if poll < len(outcomes) else "none"
        if outcome == "stopped":
            return True
        if outcome == "error":
            return False
        if outcome not in ("none", "eintr"):
            raise ValueError(f"unknown wait outcome: {outcome}")
    return False


def intentional_interrupt_stop_accepts(*, stopped: bool, signal: int,
                                       ptrace_event: int,
                                       dr6_low_nibble: int) -> bool:
    return (stopped and signal == 5 and ptrace_event == 128 and
            dr6_low_nibble == 0)


class CleanupDecisionTest(unittest.TestCase):
    def test_complete_freeze_allows_shadow_rollback(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=True, current=Vptr.SHADOW,
                           owner_stopped=True, other_threads_stopped=True,
                           all_live_threads_stopped=True),
            Decision.CONDITIONAL_ROLLBACK,
        )

    def test_owner_stop_failure_forbids_rollback(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=True, current=Vptr.SHADOW,
                           owner_stopped=False, other_threads_stopped=True,
                           all_live_threads_stopped=False),
            Decision.FORCE_STOP_FRESH_PROCESS,
        )

    def test_other_thread_stop_failure_forbids_rollback(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=True, current=Vptr.SHADOW,
                           owner_stopped=True, other_threads_stopped=False,
                           all_live_threads_stopped=False),
            Decision.FORCE_STOP_FRESH_PROCESS,
        )

    def test_inconsistent_freeze_summary_forbids_rollback(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=True, current=Vptr.SHADOW,
                           owner_stopped=True, other_threads_stopped=True,
                           all_live_threads_stopped=False),
            Decision.FORCE_STOP_FRESH_PROCESS,
        )

    def test_original_vptr_never_needs_a_write(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=True, current=Vptr.ORIGINAL,
                           owner_stopped=False, other_threads_stopped=False,
                           all_live_threads_stopped=False),
            Decision.ALREADY_RESTORED,
        )

    def test_unknown_vptr_is_never_repaired(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=True, current=Vptr.UNKNOWN,
                           owner_stopped=True, other_threads_stopped=True,
                           all_live_threads_stopped=True),
            Decision.FORCE_STOP_FRESH_PROCESS,
        )

    def test_read_failure_does_not_authorize_write(self) -> None:
        self.assertIs(
            decide_cleanup(read_ok=False, current=Vptr.SHADOW,
                           owner_stopped=True, other_threads_stopped=True,
                           all_live_threads_stopped=True),
            Decision.READ_FAILED,
        )


class FinalSnapshotBarrierModelTest(unittest.TestCase):
    def test_clean_interrupt_barrier_is_accepted(self) -> None:
        self.assertTrue(final_barrier_accepts(
            before_tids={10, 11, 12}, after_tids={10, 11, 12}, owner_dr6=1,
            other_stops=[(True, 128, 0), (True, 128, 0)],
        ))

    def test_pending_action_hwbp_is_rejected(self) -> None:
        self.assertFalse(final_barrier_accepts(
            before_tids={10, 11}, after_tids={10, 11}, owner_dr6=1,
            other_stops=[(True, 0, 0x4)],
        ))

    def test_pending_nitro_hwbp_is_rejected(self) -> None:
        self.assertFalse(final_barrier_accepts(
            before_tids={10, 11}, after_tids={10, 11}, owner_dr6=1,
            other_stops=[(True, 0, 0x2)],
        ))

    def test_thread_set_drift_is_rejected(self) -> None:
        self.assertFalse(final_barrier_accepts(
            before_tids={10, 11}, after_tids={10, 11, 12}, owner_dr6=1,
            other_stops=[(True, 128, 0)],
        ))

    def test_wrong_owner_hit_is_rejected(self) -> None:
        self.assertFalse(final_barrier_accepts(
            before_tids={10, 11}, after_tids={10, 11}, owner_dr6=0x9,
            other_stops=[(True, 128, 0)],
        ))


class PartialTransitionCleanupTest(unittest.TestCase):
    def test_partial_rearm_keeps_all_threads_stopped_for_restore(self) -> None:
        self.assertIs(
            transition_cleanup(
                any_thread_running=False, refreeze_ok=False,
                restore_detach_ok=True, process_alive=True,
                tracer_clear=True,
            ),
            CleanupDisposition.CLEAN,
        )

    def test_partial_resume_requires_and_accepts_complete_refreeze(self) -> None:
        self.assertIs(
            transition_cleanup(
                any_thread_running=True, refreeze_ok=True,
                restore_detach_ok=True, process_alive=True,
                tracer_clear=True,
            ),
            CleanupDisposition.CLEAN,
        )

    def test_partial_resume_refreeze_failure_forces_stop(self) -> None:
        self.assertIs(
            transition_cleanup(
                any_thread_running=True, refreeze_ok=False,
                restore_detach_ok=True, process_alive=True,
                tracer_clear=True,
            ),
            CleanupDisposition.FORCE_STOP_FRESH_PROCESS,
        )

    def test_restore_or_detach_failure_forces_stop(self) -> None:
        self.assertIs(
            transition_cleanup(
                any_thread_running=False, refreeze_ok=True,
                restore_detach_ok=False, process_alive=True,
                tracer_clear=True,
            ),
            CleanupDisposition.FORCE_STOP_FRESH_PROCESS,
        )

    def test_survival_or_tracer_failure_forces_stop(self) -> None:
        for process_alive, tracer_clear in ((False, True), (True, False)):
            with self.subTest(process_alive=process_alive,
                              tracer_clear=tracer_clear):
                self.assertIs(
                    transition_cleanup(
                        any_thread_running=False, refreeze_ok=True,
                        restore_detach_ok=True, process_alive=process_alive,
                        tracer_clear=tracer_clear,
                    ),
                    CleanupDisposition.FORCE_STOP_FRESH_PROCESS,
                )

class Dr7EncodingTest(unittest.TestCase):
    def test_four_slot_write_lengths_decode_independently(self) -> None:
        dr7 = 0xD9950055
        self.assertEqual(
            [decode_dr7_slot(dr7, slot) for slot in range(4)],
            [
                (True, False, 1, 2),
                (True, False, 1, 8),
                (True, False, 1, 8),
                (True, False, 1, 4),
            ],
        )


class BoundedStopWaitTest(unittest.TestCase):
    def test_stop_before_deadline_is_accepted(self) -> None:
        self.assertTrue(bounded_wait_accepts(
            ["none", "eintr", "none", "stopped"], deadline_polls=4
        ))

    def test_missing_stop_cannot_outlive_deadline(self) -> None:
        self.assertFalse(bounded_wait_accepts(
            ["none"] * 20, deadline_polls=5
        ))

    def test_eintr_retries_but_does_not_extend_deadline(self) -> None:
        self.assertFalse(bounded_wait_accepts(
            ["eintr"] * 20, deadline_polls=5
        ))

    def test_hard_wait_error_fails_immediately(self) -> None:
        self.assertFalse(bounded_wait_accepts(
            ["none", "error", "stopped"], deadline_polls=5
        ))

    def test_clean_interrupt_stop_is_accepted(self) -> None:
        self.assertTrue(intentional_interrupt_stop_accepts(
            stopped=True, signal=5, ptrace_event=128, dr6_low_nibble=0
        ))

    def test_lifecycle_event_cannot_masquerade_as_interrupt_stop(self) -> None:
        self.assertFalse(intentional_interrupt_stop_accepts(
            stopped=True, signal=5, ptrace_event=3, dr6_low_nibble=0
        ))

    def test_pending_hwbp_cannot_be_consumed_by_freeze(self) -> None:
        self.assertFalse(intentional_interrupt_stop_accepts(
            stopped=True, signal=5, ptrace_event=128, dr6_low_nibble=4
        ))

    def test_wrong_signal_or_nonstop_is_rejected(self) -> None:
        for stopped, signal in ((False, 5), (True, 19)):
            with self.subTest(stopped=stopped, signal=signal):
                self.assertFalse(intentional_interrupt_stop_accepts(
                    stopped=stopped, signal=signal, ptrace_event=128,
                    dr6_low_nibble=0,
                ))

class SourceBindingTest(unittest.TestCase):
    def test_model_is_bound_to_compiled_successor_branch(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        owner = source.index("const bool owner_stopped_for_rollback")
        others = source.index("const bool others_frozen", owner)
        all_live = source.index("AllLiveThreadsStopped(threads)", others)
        gate = source.index(
            "rollback_vptr == report.shadow_vptr && fc3_rollback_frozen",
            all_live,
        )
        rollback = source.index("ConditionalRollback(mem", gate)
        fail_closed = source.index("report.cleanup_disposition = 1", rollback)
        self.assertLess(owner, others)
        self.assertLess(others, all_live)
        self.assertLess(all_live, gate)
        self.assertLess(gate, rollback)
        self.assertLess(rollback, fail_closed)
        self.assertIn(
            "fc3_rollback_frozen = owner_stopped_for_rollback && others_frozen &&",
            source,
        )
        self.assertIn(
            "Never repair a target vptr while any live thread may still be running",
            source,
        )

    def test_kernel_clone_events_close_the_scan_only_gap(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        seize = source.index("PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL")
        preowner_wait = source.index(
            "waitpid(-1, &status, __WALL | WNOHANG)", seize
        )
        preowner_event = source.index(
            "preowner_ptrace_event != 0", preowner_wait
        )
        preowner_reject = source.index(
            "kRejectThreadLifecycle", preowner_event
        )
        preowner_force_stop = source.index(
            "cleanup_disposition = 1", preowner_reject
        )
        transaction_wait = source.index(
            "waitpid(-1, &status, __WALL | WNOHANG)", preowner_wait + 1
        )
        transaction_event = source.index(
            "ptrace_event != 0", transaction_wait
        )
        transaction_reject = source.index(
            "kRejectThreadLifecycle", transaction_event
        )
        transaction_force_stop = source.index(
            "cleanup_disposition = 1", transaction_reject
        )
        self.assertLess(seize, preowner_wait)
        self.assertLess(preowner_wait, preowner_event)
        self.assertLess(preowner_event, preowner_reject)
        self.assertLess(preowner_reject, preowner_force_stop)
        self.assertLess(preowner_force_stop, transaction_wait)
        self.assertLess(transaction_wait, transaction_event)
        self.assertLess(transaction_event, transaction_reject)
        self.assertLess(transaction_reject, transaction_force_stop)

    def test_owner_pointer_is_reacquired_after_vector_growth(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        freeze = source.index("const bool initial_frozen = FreezeAllExcept(")
        reacquire = source.index(
            "owner = FindWatched(&threads, transaction_tid)", freeze
        )
        owner_use = source.index("ContinueOwner(owner, &report)", reacquire)
        self.assertLess(freeze, reacquire)
        self.assertLess(reacquire, owner_use)
        rollback_reacquire = source.index(
            "owner = FindWatched(&threads, transaction_tid)", owner_use
        )
        rollback_tid = source.index(
            "const pid_t rollback_owner_tid = transaction_tid",
            rollback_reacquire,
        )
        self.assertLess(rollback_reacquire, rollback_tid)

    def test_post_transition_cleanup_failures_select_force_stop(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        detach = source.index("const bool detach_clean")
        detach_failure = source.index(
            "fc3_tail.report.cleanup_disposition = 1", detach
        )
        survival = source.index(
            "if (!process_alive || !tracer_clear)", detach_failure
        )
        survival_failure = source.index(
            "fc3_tail.report.cleanup_disposition = 1", survival
        )
        self.assertLess(detach, detach_failure)
        self.assertLess(detach_failure, survival)
        self.assertLess(survival, survival_failure)

    def test_final_snapshot_has_a_pending_event_barrier(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        candidate = source.index("const bool final_snapshot_candidate")
        barrier = source.index("Fc3FinalSnapshotBarrier(", candidate)
        snapshot = source.index("BuildFc3Stop(", barrier)
        consume = source.index("fc3_tail.core->Consume", snapshot)
        self.assertLess(candidate, barrier)
        self.assertLess(barrier, snapshot)
        self.assertLess(snapshot, consume)
        self.assertIn("event != PTRACE_EVENT_STOP", source)
        self.assertIn("(dr6 & 0xFu) != 0", source)
        self.assertIn("(owner_dr6 & 0xFu) == 0x1u", source)

    def test_all_candidate_stop_waits_are_bounded(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        wait_loop = source.index("while (MonotonicNs() < deadline_ns)")
        nonblocking_wait = source.index(
            "waitpid(tid, &status, __WALL | WNOHANG)", wait_loop
        )
        interrupt = source.index("Fc3InterruptAndWaitUntil", nonblocking_wait)
        self.assertLess(wait_loop, nonblocking_wait)
        self.assertLess(nonblocking_wait, interrupt)
        self.assertNotIn("waitpid(tid, &status, __WALL)", source)
        self.assertIn("errno != EINTR", source)
        self.assertIn("Fc3StopThreadUntil", source)
        self.assertIn("WSTOPSIG(status) != SIGTRAP", source)
        self.assertIn(") != PTRACE_EVENT_STOP", source)
        stop_until = source.index("bool Fc3StopThreadUntil")
        stop_dr6 = source.index("(dr6 & 0xFu) == 0", stop_until)
        stop_bounded = source.index("bool Fc3StopThreadBounded", stop_dr6)
        self.assertLess(stop_until, stop_dr6)
        self.assertLess(stop_dr6, stop_bounded)

    def test_freeze_cleanup_and_final_barrier_share_deadlines(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        self.assertEqual(
            source.count(
                "A9TAS_STOP_THREAD_UNTIL(thread.tid, stop_deadline)"
            ),
            3,
        )
        self.assertGreaterEqual(
            source.count(
                "const std::uint64_t stop_deadline = "
                "MonotonicNs() + kFc3StopWaitNs"
            ),
            2,
        )
        self.assertIn(
            "stop_deadline_ns = MonotonicNs() + kFc3StopWaitNs", source
        )
        attach_rescan = source.index(
            "AttachCurrentThreads(pid, flags_address, false, threads"
        )
        attach_shared_deadline = source.index(
            ", stop_deadline", attach_rescan
        )
        self.assertLess(attach_rescan, attach_shared_deadline)
        barrier_signature = source.index(
            "bool Fc3FinalSnapshotBarrier(pid_t pid, pid_t owner,"
        )
        barrier_deadline = source.index(
            "std::uint64_t deadline_ns", barrier_signature
        )
        barrier_wait = source.index(
            "Fc3InterruptAndWaitUntil(thread.tid, deadline_ns", barrier_deadline
        )
        barrier_call = source.index(
            "Fc3FinalSnapshotBarrier(pid, transaction_tid", barrier_wait
        )
        transaction_deadline = source.index(
            "transaction_deadline, &threads", barrier_call
        )
        self.assertLess(barrier_signature, barrier_deadline)
        self.assertLess(barrier_deadline, barrier_wait)
        self.assertLess(barrier_wait, barrier_call)
        self.assertLess(barrier_call, transaction_deadline)


if __name__ == "__main__":
    unittest.main()
