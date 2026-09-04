#!/usr/bin/env python3
"""Offline policy checks for the fail-closed FC-3 phase-map runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-fc3-phase-map-v1.ps1").read_text(encoding="utf-8")
CORE = (ROOT / "src" / "fc3_phase_map_v1.cpp").read_text(encoding="utf-8")
CONTROLLER = (ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp").read_text(encoding="utf-8")


class PhaseMapRunnerPolicyTests(unittest.TestCase):
    def test_offline_is_default_and_returns_before_adb_validation(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', RUNNER)
        offline = RUNNER.index('if ($Mode -eq "OfflineValidateOnly")')
        adb = RUNNER.index('if (-not (Test-Path -LiteralPath $AdbPath')
        self.assertLess(offline, adb)
        self.assertIn("device_access=0", RUNNER[offline:adb])

    def test_prepare_and_probe_have_separate_acknowledgements(self) -> None:
        for token in (
            "$AcknowledgeFreshGameProcessRestart",
            "$AcknowledgePreloadWindowInjection",
            "$AcknowledgePassiveFc2PayloadOnly",
            "$AcknowledgeNaturallyRunningRace",
            "$AcknowledgeNoPausedAttach",
            "$AcknowledgeFc2Fc3PhaseMap",
            "$AcknowledgeOneVptrSwapAndGameOwnedAddRemove",
            "$AcknowledgeNoActionInputNitroOrPhysicsWrite",
            "$AcknowledgeFourHardwareWatchpointsAndPtraceStalls",
            "$AcknowledgeExitKillMayTerminateFreshProcess",
            "$AcknowledgeFailureForceStopsFreshProcess",
        ):
            self.assertIn(token, RUNNER)

    def test_pins_complete_artifact_closure(self) -> None:
        for token in (
            "$payload =", "$bootstrap =", "$controller =", "$injector =",
            "$helper =", "$fc2Validator =", "$phaseValidator =",
            "Assert-LocalArtifacts", "Assert-RemoteHash",
        ):
            self.assertIn(token, RUNNER)
        self.assertGreaterEqual(RUNNER.count("sha256"), 8)
        self.assertIn(
            '$remoteInjector = "/data/local/tmp/a9tas_injector_fc2_v1"',
            RUNNER,
        )

    def test_receipt_binds_boot_process_nonce_and_ttl(self) -> None:
        for token in (
            "boot_id", "start_time", "created_unix", "ReceiptTtlSeconds",
            "^[0-9a-f]{32}$", "Get-BootId", "Get-ProcessStartTime",
        ):
            self.assertIn(token, RUNNER)

    def test_cleanup_is_armed_before_receipt_parse(self) -> None:
        armed = RUNNER.index("$cleanupArmed = $true")
        parsed = RUNNER.index("ConvertFrom-Json", armed)
        self.assertLess(armed, parsed)
        tail = RUNNER[parsed:]
        self.assertIn("if ($cleanupArmed -and -not $probePassed)", tail)
        self.assertIn("Stop-FreshPackage", tail)

    def test_single_exact_controller_invocation_and_ack(self) -> None:
        self.assertEqual(RUNNER.count("$remoteController $gamePid $startTime"), 1)
        self.assertIn("I_ACCEPT_FC3_PHASE_MAP_OBSERVE_ONLY_V1", RUNNER)
        self.assertIn("FC3_PHASE_MAP_DONE success=1", RUNNER)

    def test_both_reports_are_hash_bound_and_strictly_validated(self) -> None:
        for token in (
            "$remoteFc2Hash", "$remotePhaseHash", "$localFc2", "$localPhase",
            "--fc2-report $localFc2", "Remote/local report hash mismatch",
        ):
            self.assertIn(token, RUNNER)
        self.assertIn(
            "phase_order=DT-C98-open-C9C-F64-dedicated-close-world-nextDT-nextC98",
            RUNNER,
        )

    def test_failed_reports_are_preserved_before_controller_rejection(self) -> None:
        chmod = RUNNER.index("chmod 0444 $remote")
        pull_fc2 = RUNNER.index("'pull', $remoteFc2", chmod)
        hash_check = RUNNER.index("Remote/local report hash mismatch", pull_fc2)
        preserved = RUNNER.index("FC3_PHASE_MAP_FAILED_REPORTS_PRESERVED", hash_check)
        rejected = RUNNER.index("Phase-map controller failed closed", preserved)
        self.assertLess(chmod, pull_fc2)
        self.assertLess(pull_fc2, hash_check)
        self.assertLess(hash_check, preserved)
        self.assertLess(preserved, rejected)

    def test_no_input_or_gameplay_action_transport(self) -> None:
        lower = RUNNER.lower()
        for forbidden in ("keyevent", "input tap", "sendevent", "game_action_rpc", "nitro_rpc"):
            self.assertNotIn(forbidden, lower)

    def test_phase_map_directly_validates_physics_boundaries(self) -> None:
        for token in (
            "post_close_deferred_clear_seen_",
            "kRejectMissingDeferredClear",
            "FiniteFloatBits(event.observed_bits, 1000000.0f)",
            "FiniteFloatBits(event.observed_bits, 60.0f)",
            "event.tid == callback_owner_tid_",
        ):
            self.assertIn(token, CORE)
        self.assertGreaterEqual(CONTROLLER.count("&phase_event.observed_bits"), 2)

    def test_phase_map_safely_enrolls_kernel_traced_clones(self) -> None:
        for token in (
            "IncorporateAutoTracedClone",
            "PTRACE_GETEVENTMSG",
            "WaitForAutoTracedChild",
            "PTRACE_EVENT_CLONE",
            "clean_state = parent->original",
            "owner = FindWatched(&threads, transaction_tid)",
        ):
            self.assertIn(token, CONTROLLER)

    def test_initial_attach_tolerates_only_confirmed_retired_tasks(self) -> None:
        for token in (
            "Fc3TaskIsGone(pid, tid)",
            '"/proc/%d/task/%d"',
            "access(task_path, F_OK) == -1 && errno == ENOENT",
            "stage=initial_attach_retired",
            "stage=attach_current_failed",
        ):
            self.assertIn(token, CONTROLLER)
        retired = CONTROLLER.index("stage=initial_attach_retired")
        continued = CONTROLLER.index("continue;", retired)
        failed = CONTROLLER.index("stage=attach_current_failed", continued)
        self.assertLess(retired, continued)
        self.assertLess(continued, failed)

    def test_retirement_accounting_distinguishes_tracked_and_untracked(self) -> None:
        for token in (
            "untracked_retired_threads = 0",
            "threads.size() +\n                                     untracked_retired_threads",
            "+ untracked_retired_threads",
            "cleanup_tracked_retired",
            "verified_clean.valid = true",
        ):
            self.assertIn(token, CONTROLLER)

        # The compiled controller uses this invariant: tracked retirements are
        # already represented by the enrolled vector; only never-enrolled
        # retirements extend the right-hand side.
        cases = (
            # enrolled, detached, all retired, untracked retired
            (282, 282, 0, 0),
            (282, 281, 1, 0),
            (282, 282, 1, 1),
            (282, 281, 2, 1),
            (283, 283, 0, 0),  # pending stopped child enrolled at cleanup
        )
        for enrolled, detached, retired_count, untracked in cases:
            self.assertEqual(
                detached + retired_count,
                enrolled + untracked,
            )

    def test_initial_enrollment_converges_while_existing_parents_are_stopped(self) -> None:
        for token in (
            "AttachStableInitialThreadSet",
            "AttachCurrentThreads(pid, flags_address, false, threads,",
            "threads->size() == before && Fc3ThreadSetMatches(pid, *threads)",
            "stage=initial_stable_attached",
            "stage=initial_stable_not_converged",
        ):
            self.assertIn(token, CONTROLLER)
        helper = CONTROLLER.index("bool AttachStableInitialThreadSet")
        stopped_attach = CONTROLLER.index(
            "AttachCurrentThreads(pid, flags_address, false, threads,",
            helper,
        )
        matched = CONTROLLER.index("Fc3ThreadSetMatches(pid, *threads)", stopped_attach)
        resumed = CONTROLLER.index("ContinueThread(thread.tid)", matched)
        self.assertLess(stopped_attach, matched)
        self.assertLess(matched, resumed)

    def test_freeze_accepts_only_proved_clean_thread_retirement(self) -> None:
        for token in (
            "enum class Fc3FreezeStopResult",
            "Fc3FreezeThreadUntil",
            "WIFEXITED(status) && WEXITSTATUS(status) == 0",
            "Fc3TaskIsGone(pid, tid)",
            "Fc3FreezeStopResult::kCleanRetired",
            "freeze_existing_clean_retired",
            "freeze_new_clean_retired",
            "thread.live = false",
            "++*retired_threads",
            "&threads, &retired_threads",
        ):
            self.assertIn(token, CONTROLLER)

        helper = CONTROLLER.index("Fc3FreezeStopResult Fc3FreezeThreadUntil")
        interrupted = CONTROLLER.index("ptrace(PTRACE_INTERRUPT", helper)
        waited = CONTROLLER.index("Fc3WaitForStopUntil", interrupted)
        clean_exit = CONTROLLER.index("WIFEXITED(status)", waited)
        zero_exit = CONTROLLER.index("WEXITSTATUS(status) == 0", clean_exit)
        gone = CONTROLLER.index("Fc3TaskIsGone(pid, tid)", zero_exit)
        clean_result = CONTROLLER.index(
            "return Fc3FreezeStopResult::kCleanRetired", gone
        )
        stopped_check = CONTROLLER.index("!WIFSTOPPED(status)", clean_result)
        self.assertLess(interrupted, waited)
        self.assertLess(waited, clean_exit)
        self.assertLess(clean_exit, zero_exit)
        self.assertLess(zero_exit, gone)
        self.assertLess(gone, clean_result)
        self.assertLess(clean_result, stopped_check)

        # Unknown status, a non-zero exit, signal death, a task that still
        # exists, or a stop carrying breakpoint evidence must remain fatal.
        def classify(*, exited: bool, exit_code: int, gone: bool,
                     stopped: bool, trap: bool, event_stop: bool,
                     dr6_clear: bool) -> str:
            if exited and exit_code == 0 and gone:
                return "retired"
            if stopped and trap and event_stop and dr6_clear:
                return "stopped"
            return "failed"

        self.assertEqual(classify(exited=True, exit_code=0, gone=True,
                                  stopped=False, trap=False, event_stop=False,
                                  dr6_clear=False), "retired")
        for case in (
            dict(exited=True, exit_code=1, gone=True, stopped=False,
                 trap=False, event_stop=False, dr6_clear=False),
            dict(exited=False, exit_code=0, gone=True, stopped=False,
                 trap=False, event_stop=False, dr6_clear=False),
            dict(exited=True, exit_code=0, gone=False, stopped=False,
                 trap=False, event_stop=False, dr6_clear=False),
            dict(exited=False, exit_code=0, gone=False, stopped=True,
                 trap=True, event_stop=True, dr6_clear=False),
        ):
            self.assertEqual(classify(**case), "failed")

    def test_phase_transaction_deadline_is_independent_of_owner_search(self) -> None:
        marker = CONTROLLER.index("const bool transaction_deadline_valid")
        branch_start = CONTROLLER.rfind(
            "#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1", 0, marker
        )
        branch_else = CONTROLLER.index("#else", marker)
        branch_end = CONTROLLER.index("#endif", branch_else)
        phase_branch = CONTROLLER[branch_start:branch_else]
        legacy_branch = CONTROLLER[branch_else:branch_end]

        for token in (
            "UINT64_MAX - kFc3PhaseMapTransactionDeadlineNs",
            "report.bootstrap_open_ns +",
            "kFc3PhaseMapTransactionDeadlineNs",
            "stage=transaction_window",
            "owner_deadline=",
            "phase_deadline=",
        ):
            self.assertIn(token, phase_branch)
        self.assertNotIn("std::min", phase_branch)
        self.assertIn("std::min<std::uint64_t>", legacy_branch)
        self.assertIn("wait_deadline", legacy_branch)

        # A late owner selection must still receive the complete phase window;
        # the old min(owner_deadline, phase_deadline) behavior would expire it.
        owner_search_deadline = 1_500_000_000
        bootstrap_open = 1_450_000_000
        phase_window = 3_000_000_000
        self.assertEqual(bootstrap_open + phase_window, 4_450_000_000)
        self.assertGreater(bootstrap_open + phase_window, owner_search_deadline)

    def test_clean_retirement_propagates_through_watchpoint_consumers(self) -> None:
        arm_start = CONTROLLER.index("bool ArmFc3BeforeDedicated")
        arm_end = CONTROLLER.index("bool RearmFc3NitroAndResumeOthers", arm_start)
        arm = CONTROLLER[arm_start:arm_end]
        self.assertIn("if (!thread.live) continue;", arm)
        self.assertIn("stage=arm_before_dedicated_failed", arm)
        self.assertLess(
            arm.index("if (!thread.live) continue;"),
            arm.index("!ArmFc3WatchPlan"),
        )

        rearm_start = CONTROLLER.index("bool RearmFc3PhaseMapAndResumeOthers")
        rearm_end = CONTROLLER.index("bool ArmDedicatedWatch", rearm_start)
        rearm = CONTROLLER[rearm_start:rearm_end]
        self.assertIn("if (!thread.live) continue;", rearm)
        self.assertIn("stage=rearm_phase_map_failed", rearm)
        self.assertLess(
            rearm.index("if (!thread.live) continue;"),
            rearm.index("!ArmFc3WatchPlan"),
        )
        self.assertIn(
            'ResumeAllLiveExcept(threads, stopped_tid, "rearm_phase_map")',
            rearm,
        )

    def test_initial_transaction_resumes_workers_before_owner(self) -> None:
        for token in (
            "bool ResumeAllLiveExcept",
            "if (!thread.live) continue;",
            "if (!thread.stopped)",
            "if (!ContinueThread(thread.tid))",
            "stopped_tid_found",
            "stage=%s_anchor_not_stopped",
            "stage=%s_worker_not_stopped",
            "stage=%s_resume_others_failed",
            '"initial_transaction"',
            '"rearm_phase_map"',
        ):
            self.assertIn(token, CONTROLLER)
        self.assertEqual(CONTROLLER.count("ResumeAllLiveExcept("), 3)

        main_install = CONTROLLER.index(
            "} else if (InstallShadowVptrWhileFrozen"
        )
        resume_workers = CONTROLLER.index(
            "ResumeAllLiveExcept(&threads, transaction_tid",
            main_install,
        )
        continue_owner = CONTROLLER.index(
            "if (ContinueOwner(owner, &report))", resume_workers
        )
        self.assertLess(main_install, resume_workers)
        self.assertLess(resume_workers, continue_owner)

        # Model the ledger invariant across initial arm, a later rearm with a
        # clean retirement, and cleanup. Retired entries remain enrolled for
        # accounting, while every live worker runs between phase barriers.
        ledger = {
            10: {"live": True, "stopped": True},   # owner
            11: {"live": True, "stopped": True},
            12: {"live": True, "stopped": True},
            13: {"live": False, "stopped": False},  # proved retired
        }

        def resume_all_except(anchor: int) -> bool:
            if anchor not in ledger or not ledger[anchor]["live"] or not ledger[anchor]["stopped"]:
                return False
            for tid, state in ledger.items():
                if not state["live"] or tid == anchor:
                    continue
                if not state["stopped"]:
                    return False
                state["stopped"] = False
            return True

        self.assertTrue(resume_all_except(10))
        ledger[10]["stopped"] = False
        self.assertTrue(all(not state["stopped"] for state in ledger.values()))

        # At the next phase barrier all live entries freeze; one cleanly exits.
        for state in ledger.values():
            if state["live"]:
                state["stopped"] = True
        ledger[12] = {"live": False, "stopped": False}
        self.assertTrue(resume_all_except(10))
        ledger[10]["stopped"] = False
        self.assertTrue(all(
            (not state["live"]) or (not state["stopped"])
            for state in ledger.values()
        ))
        detached = sum(1 for state in ledger.values() if state["live"])
        retired = sum(1 for state in ledger.values() if not state["live"])
        self.assertEqual(detached + retired, len(ledger))

    def test_partial_resume_failure_is_recoverable_by_terminal_freeze(self) -> None:
        # Validate every live task before the first continue so a malformed
        # ledger cannot cause a preventable partial release.
        helper_start = CONTROLLER.index("bool ResumeAllLiveExcept")
        helper_end = CONTROLLER.index("#endif", helper_start)
        helper = CONTROLLER[helper_start:helper_end]
        validation_loop = helper.index("for (const auto& thread")
        anchor_check = helper.index("if (!stopped_tid_found)", validation_loop)
        resume_loop = helper.index("for (auto& thread", anchor_check)
        continue_call = helper.index("ContinueThread(thread.tid)", resume_loop)
        self.assertLess(validation_loop, anchor_check)
        self.assertLess(anchor_check, resume_loop)
        self.assertLess(resume_loop, continue_call)

        def attempt_then_rollback(fail_tid: int | None) -> tuple[bool, list[dict[str, object]]]:
            ledger: list[dict[str, object]] = [
                {"tid": 10, "live": True, "stopped": True},
                {"tid": 11, "live": True, "stopped": True},
                {"tid": 12, "live": True, "stopped": True},
                {"tid": 13, "live": False, "stopped": False},
            ]
            success = True
            for state in ledger:
                if not state["live"] or state["tid"] == 10:
                    continue
                if state["tid"] == fail_tid:
                    success = False
                    break
                state["stopped"] = False

            if not success:
                # The owner is still stopped. Terminal rollback interrupts
                # every worker that was already resumed; retired entries stay
                # out of the kernel operation set.
                for state in ledger:
                    if state["live"]:
                        state["stopped"] = True
            return success, ledger

        for failure in (11, 12):
            success, ledger = attempt_then_rollback(failure)
            self.assertFalse(success)
            self.assertTrue(all(
                (not state["live"]) or state["stopped"]
                for state in ledger
            ))
        success, ledger = attempt_then_rollback(None)
        self.assertTrue(success)
        self.assertTrue(ledger[0]["stopped"])
        self.assertTrue(all(
            (not state["live"]) or state["tid"] == 10 or not state["stopped"]
            for state in ledger
        ))

    def test_every_phase_freeze_has_an_explicit_release_or_terminal_cleanup(self) -> None:
        # Definition plus exactly three call sites: initial installation,
        # phase rearm, and terminal rollback.  Adding another freeze site must
        # force this lifecycle audit to be updated instead of silently leaving
        # live tasks stopped.
        self.assertEqual(CONTROLLER.count("FreezeAllExcept("), 4)

        initial = CONTROLLER.index(
            "const bool initial_frozen = FreezeAllExcept(\n"
            "        pid, transaction_tid, report.callback_flags"
        )
        initial_resume = CONTROLLER.index(
            'ResumeAllLiveExcept(&threads, transaction_tid,\n'
            '                                 "initial_transaction")',
            initial,
        )
        initial_owner = CONTROLLER.index(
            "if (ContinueOwner(owner, &report))", initial_resume
        )
        self.assertLess(initial, initial_resume)
        self.assertLess(initial_resume, initial_owner)

        rearm = CONTROLLER.index("bool RearmFc3PhaseMapAndResumeOthers")
        rearm_freeze = CONTROLLER.index("FreezeAllExcept(", rearm)
        rearm_workers = CONTROLLER.index(
            'ResumeAllLiveExcept(threads, stopped_tid, "rearm_phase_map")',
            rearm_freeze,
        )
        phase_dispatch = CONTROLLER.index(
            "if (must_rearm) {", rearm_workers
        )
        event_resume = CONTROLLER.index(
            "!ContinueThread(event_thread->tid)", phase_dispatch
        )
        owner_resume = CONTROLLER.index(
            "if (await != Await::kDone && !ContinueOwner(owner, &report))",
            event_resume,
        )
        self.assertLess(rearm_freeze, rearm_workers)
        self.assertLess(rearm_workers, phase_dispatch)
        self.assertLess(phase_dispatch, event_resume)
        self.assertLess(event_resume, owner_resume)

        rollback = CONTROLLER.index(
            "const bool others_frozen = FreezeAllExcept(", owner_resume
        )
        all_stopped = CONTROLLER.index("AllLiveThreadsStopped(threads)", rollback)
        restore_detach = CONTROLLER.index("RestoreAndDetachAll(&threads", all_stopped)
        self.assertLess(rollback, all_stopped)
        self.assertLess(all_stopped, restore_detach)

    def test_phase_rearm_reacquires_vector_pointers_after_possible_growth(self) -> None:
        dispatch = CONTROLLER.index("if (must_rearm) {")
        stable_event_tid = CONTROLLER.rfind(
            "const pid_t phase_event_tid = event_thread->tid", 0, dispatch
        )
        rearm = CONTROLLER.index(
            "RearmFc3PhaseMapAndResumeOthers(\n"
            "                      pid, phase_event_tid",
            dispatch,
        )
        event_reacquire = CONTROLLER.index(
            "event_thread = FindWatched(&threads, phase_event_tid)", rearm
        )
        owner_reacquire = CONTROLLER.index(
            "owner = FindWatched(&threads, transaction_tid)", event_reacquire
        )
        state_check = CONTROLLER.index(
            "owner->stopped != owner_should_be_stopped", owner_reacquire
        )
        event_resume = CONTROLLER.index(
            "!ContinueThread(event_thread->tid)", state_check
        )
        owner_resume = CONTROLLER.index(
            "if (await != Await::kDone && !ContinueOwner(owner, &report))",
            event_resume,
        )

        self.assertLess(stable_event_tid, dispatch)
        self.assertLess(dispatch, rearm)
        self.assertLess(rearm, event_reacquire)
        self.assertLess(event_reacquire, owner_reacquire)
        self.assertLess(owner_reacquire, state_check)
        self.assertLess(state_check, event_resume)
        self.assertLess(state_check, owner_resume)
        self.assertIn("stage=post_rearm_reacquire_failed", CONTROLLER)

        # std::vector growth invalidates element pointers, while the pid value
        # remains stable.  This executable model guards the exact failure mode
        # that the source ordering above prevents.
        threads = [
            {"tid": 10, "live": True, "stopped": False},
            {"tid": 11, "live": True, "stopped": True},
        ]
        event_tid = threads[1]["tid"]
        owner_tid = threads[0]["tid"]
        stale_event_index = 1
        threads.insert(0, {"tid": 12, "live": True, "stopped": False})
        self.assertNotEqual(threads[stale_event_index]["tid"], event_tid)
        event = next(item for item in threads if item["tid"] == event_tid)
        owner = next(item for item in threads if item["tid"] == owner_tid)
        self.assertTrue(event["live"] and event["stopped"])
        self.assertTrue(owner["live"] and not owner["stopped"])

    def test_threads_enrolled_during_freeze_extend_both_report_ledgers(self) -> None:
        rearm = CONTROLLER.index("bool RearmFc3PhaseMapAndResumeOthers")
        rearm_end = CONTROLLER.index("bool ArmDedicatedWatch", rearm)
        rearm_body = CONTROLLER[rearm:rearm_end]
        for token in (
            "const std::size_t enrolled_before = threads->size()",
            "const std::size_t newly_enrolled = threads->size() - enrolled_before",
            "UINT32_MAX - report->initial_threads",
            "UINT32_MAX - tail->phase_report.initial_threads",
            "report->initial_threads +=",
            "tail->phase_report.initial_threads +=",
        ):
            self.assertIn(token, rearm_body)

        rollback = CONTROLLER.index(
            "const std::size_t rollback_enrolled_before = threads.size()"
        )
        rollback_end = CONTROLLER.index("if (!fc3_rollback_frozen)", rollback)
        rollback_body = CONTROLLER[rollback:rollback_end]
        for token in (
            "rollback_newly_enrolled",
            "rollback_accounting_ok",
            "report.initial_threads +=",
            "fc3_tail.phase_report.initial_threads +=",
            "AllLiveThreadsStopped(threads)",
        ):
            self.assertIn(token, rollback_body)

        # New tasks found during either a rearm barrier or the terminal
        # rollback remain part of exact initial/final accounting.
        initial_fc2 = initial_fc3 = 314
        enrolled_during_rearm = 2
        enrolled_during_rollback = 1
        final_threads = 317
        initial_fc2 += enrolled_during_rearm + enrolled_during_rollback
        initial_fc3 += enrolled_during_rearm + enrolled_during_rollback
        self.assertEqual(initial_fc2, final_threads)
        self.assertEqual(initial_fc3, final_threads)

    def test_rollback_reacquires_owner_after_any_failed_vector_mutation(self) -> None:
        loop_end = CONTROLLER.index(
            "if (await != Await::kDone && report.ptrace_errors == 0"
        )
        reacquire = CONTROLLER.index(
            "owner = FindWatched(&threads, transaction_tid)", loop_end
        )
        owner_check = CONTROLLER.index("if (!owner || !owner->live)", reacquire)
        stop = CONTROLLER.index("A9TAS_STOP_THREAD(owner->tid)", owner_check)
        stable_tid = CONTROLLER.index(
            "const pid_t rollback_owner_tid = transaction_tid", stop
        )
        rollback_freeze = CONTROLLER.index("FreezeAllExcept(", stable_tid)
        self.assertLess(loop_end, reacquire)
        self.assertLess(reacquire, owner_check)
        self.assertLess(owner_check, stop)
        self.assertLess(stop, stable_tid)
        self.assertLess(stable_tid, rollback_freeze)
        self.assertIn("stage=rollback_owner_reacquire_failed", CONTROLLER)

    def test_proved_phase_hands_callback_tail_back_to_fc2(self) -> None:
        proved = CONTROLLER.index("if (fc3_tail.phase_core->proved())")
        tail_classification = CONTROLLER.index(
            "const bool fc2_tail_event", proved
        )
        callback = CONTROLLER.index("fc3_hits == 0x1u", tail_classification)
        dedicated = CONTROLLER.index(
            "fc3_hits == 0x2u && !fc3_tail.watching_world_commit",
            callback,
        )
        phase_consume = CONTROLLER.index(
            "fc3_tail.phase_core->Consume", dedicated
        )
        fc2_flags = CONTROLLER.index(
            "const bool flags_hit = (dr6 & 1UL) != 0", phase_consume
        )
        compaction = CONTROLLER.index(
            "await == Await::kCompactionOpen", fc2_flags
        )
        self.assertLess(proved, tail_classification)
        self.assertLess(tail_classification, callback)
        self.assertLess(callback, dedicated)
        self.assertLess(dedicated, phase_consume)
        self.assertLess(phase_consume, fc2_flags)
        self.assertLess(fc2_flags, compaction)
        self.assertIn(
            "Do not feed that legitimate FC2 tail\n"
            "              // event back into an already-proved FC3 core.",
            CONTROLLER,
        )

        # The combined controller must accept the valid terminal ordering:
        # FC3 proves at next-C98, then FC2 consumes the following callback open.
        phase_proved = False
        fc2_await = "compaction_open"
        for event in ("next_c98", "callback_open"):
            if not phase_proved:
                self.assertEqual(event, "next_c98")
                phase_proved = True
                continue
            self.assertEqual(fc2_await, "compaction_open")
            self.assertEqual(event, "callback_open")
            fc2_await = "done"
        self.assertTrue(phase_proved)
        self.assertEqual(fc2_await, "done")

    def test_accumulator_reset_is_not_classified_as_fixed_delta(self) -> None:
        delta_hit = CONTROLLER.index("} else if (fc3_hits == 0x4u) {")
        f64_branch = CONTROLLER.index("if (fc3_tail.watching_f64)", delta_hit)
        accumulator_read = CONTROLLER.index(
            "fc3_tail.identity.phase_witness_accumulator", f64_branch
        )
        reset_check = CONTROLLER.index("if (accumulator == 0)", accumulator_read)
        reset_continue = CONTROLLER.index(
            "ContinueThread(event_thread->tid)", reset_check
        )
        delta_kind = CONTROLLER.index(
            "a9tas::fc3_phase_map_v1::EventKind::kDelta", reset_continue
        )
        core_consume = CONTROLLER.index(
            "fc3_tail.phase_core->Consume", delta_kind
        )
        self.assertLess(delta_hit, f64_branch)
        self.assertLess(f64_branch, accumulator_read)
        self.assertLess(accumulator_read, reset_check)
        self.assertLess(reset_check, reset_continue)
        self.assertLess(reset_continue, delta_kind)
        self.assertLess(delta_kind, core_consume)
        for token in (
            "non-zero is PRE_PHYSICS/fixed delta",
            "zero is the",
            "POST_PHYSICS reset",
            "stage=accumulator_write",
        ):
            self.assertIn(token, CONTROLLER)

        # Match the already live-proved scheduler semantics: a trailing reset
        # from the prefix cycle is ignored, while the next non-zero write is
        # the single Delta delivered to the phase core.
        observed = (0, 16667)
        delivered = [value for value in observed if value != 0]
        self.assertEqual(delivered, [16667])

    def test_entry_stability_path_has_no_debug_or_game_write_transport(self) -> None:
        for token in (
            "I_ACCEPT_FC3_ENTRY_STABILITY_ATTACH_READ_DETACH_V1",
            "RunFc3EntryStability",
            "AttachStableInitialThreadSet(pid, 0, false, &threads)",
            "DetachStoppedWithoutDebugWrites",
            "debug_write_attempts == 0",
            "game_write_attempts == 0",
            "FC3_ENTRY_STABILITY_DONE",
        ):
            self.assertIn(token, CONTROLLER)
        dispatch = CONTROLLER.index("if (entry_stability_only)")
        entry_call = CONTROLLER.index("return RunFc3EntryStability", dispatch)
        mem_open = CONTROLLER.index("open(mem_path, O_RDONLY", entry_call)
        payload_prepare = CONTROLLER.index("PreparePayload", mem_open)
        self.assertLess(entry_call, mem_open)
        self.assertLess(mem_open, payload_prepare)


if __name__ == "__main__":
    unittest.main()
