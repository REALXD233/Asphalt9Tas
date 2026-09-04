#!/usr/bin/env python3
"""Emit the complete reviewed replacement plan for the barrel successor.

The generated executor remains a derivative of the hash-pinned known-good
source.  This file contains only the ten narrow integration replacements; it
does not copy or modify the canonical source tree.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ANCHORS: tuple[tuple[str, str, str], ...] = (
    (
        "include_barrel_integration",
        '''#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
#include "final_writer_unified_integration_v1.h"
#endif''',
        '''// A9TAS_GENERATED_BEGIN anchor=include_barrel_integration
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
#include "final_writer_unified_integration_v1.h"
#endif
#ifdef A9TAS_BARREL_SUCCESSOR_V1
#include "barrel_successor_runtime_v1.h"
#endif
// A9TAS_GENERATED_END anchor=include_barrel_integration''',
    ),
    (
        "runtime_capability_mask",
        '''    a9tas::unified_tick_v1::kSkipAccelerator |
    a9tas::unified_tick_v1::kSkipBarrelAngular |
    a9tas::unified_tick_v1::kSkipBarrelRbx |
    a9tas::unified_tick_v1::kSkipRespawnButton;''',
        '''// A9TAS_GENERATED_BEGIN anchor=runtime_capability_mask
    a9tas::unified_tick_v1::kSkipAccelerator |
#ifndef A9TAS_BARREL_SUCCESSOR_V1
    a9tas::unified_tick_v1::kSkipBarrelAngular |
    a9tas::unified_tick_v1::kSkipBarrelRbx |
#endif
    a9tas::unified_tick_v1::kSkipRespawnButton;
// A9TAS_GENERATED_END anchor=runtime_capability_mask''',
    ),
    (
        "barrel_setup",
        '''#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    a9tas::final_writer_unified_v1::Runtime final_writer_runtime{};
    if (!a9tas::final_writer_unified_v1::Setup(
            pid, mem, base, vehicle.physics_base,
            backend.native_pose_address, backend.native_linear_address,
            argv[4], static_cast<std::uint32_t>(frames.size()),
            &final_writer_runtime)) {
        std::fprintf(stderr,
                     "final-writer hash/source/storage setup failed before "
                     "attach\\n");
        close(mem);
        return 3;
    }
#endif''',
        '''// A9TAS_GENERATED_BEGIN anchor=barrel_setup
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    a9tas::final_writer_unified_v1::Runtime final_writer_runtime{};
    if (!a9tas::final_writer_unified_v1::Setup(
            pid, mem, base, vehicle.physics_base,
            backend.native_pose_address, backend.native_linear_address,
            argv[4], static_cast<std::uint32_t>(frames.size()),
            &final_writer_runtime)) {
        std::fprintf(stderr,
                     "final-writer hash/source/storage setup failed before "
                     "attach\\n");
        close(mem);
        return 3;
    }
#endif
#ifdef A9TAS_BARREL_SUCCESSOR_V1
    a9tas::barrel_successor_runtime_v1::Runtime barrel_runtime{};
    if (!barrel_runtime.Setup(
            pid, mem, base, backend.physics_velocity_interface,
            backend.angular_source_base, backend.native_angular_address,
            callback_flags, f64, world_accumulator, frames,
            final_writer_runtime.prepared.published_control.recording_sha256)) {
        std::fprintf(stderr,
                     "barrel successor source/payload setup failed before "
                     "attach\\n");
        close(mem);
        return 3;
    }
#endif
// A9TAS_GENERATED_END anchor=barrel_setup''',
    ),
    (
        "barrel_runtime_state",
        '''    pid_t cycle_tid = 0;
    pid_t post_phase_tid = 0;
    UnifiedFrameAuditV5 pending{};''',
        '''// A9TAS_GENERATED_BEGIN anchor=barrel_runtime_state
    pid_t cycle_tid = 0;
    pid_t post_phase_tid = 0;
#ifdef A9TAS_BARREL_SUCCESSOR_V1
    bool barrel_process_fatal = false;
    std::vector<pid_t> barrel_window_frozen_tids;
    const auto freeze_barrel_window_others = [&](pid_t owner) {
        if (owner <= 0) return false;
        if (!barrel_window_frozen_tids.empty()) return true;
        for (int pass = 0; pass < 6; ++pass) {
            for (auto& thread : threads) {
                if (!thread.live || thread.tid == owner || thread.stopped)
                    continue;
                if (!StopThread(thread.tid)) return false;
                thread.stopped = true;
                barrel_window_frozen_tids.push_back(thread.tid);
            }
            const std::size_t old_size = threads.size();
            std::uint64_t failures = 0;
            const std::size_t added = AttachNewThreadsStopped(
                pid, delta_address, c98_address, c9c_address,
                world_accumulator, &threads, &failures, BoundaryDr7(true));
            report.thread_additions += added;
            if (failures != 0) return false;
            for (std::size_t index = old_size; index < threads.size(); ++index) {
                const auto& thread = threads[index];
                if (thread.live && thread.tid != owner && thread.stopped)
                    barrel_window_frozen_tids.push_back(thread.tid);
            }
            if (added == 0) return true;
        }
        return false;
    };
    const auto resume_barrel_window_others = [&]() {
        bool ok = true;
        for (const pid_t frozen_tid : barrel_window_frozen_tids) {
            TracedThread* thread = FindThread(&threads, frozen_tid);
            if (thread == nullptr || !thread->live) continue;
            if (!thread->stopped || !ContinueThread(frozen_tid)) {
                ok = false;
            } else {
                thread->stopped = false;
            }
        }
        barrel_window_frozen_tids.clear();
        return ok;
    };
#endif
    UnifiedFrameAuditV5 pending{};
// A9TAS_GENERATED_END anchor=barrel_runtime_state''',
    ),
    (
        "c9c_barrel_layout",
        '''                                if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                    cycle_tid != 0 &&
#endif
                                    report.read_errors == 0 &&
                                    report.semantic_errors == 0 &&
                                    !ProgramStoppedThread(
                                         tid, completion, callback_flags, f64,
                                         world_accumulator, PipelineDr7())) {
                                    ++report.ptrace_errors;
                                } else if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                           cycle_tid != 0 &&
#endif
                                           report.read_errors == 0 &&
                                           report.semantic_errors == 0) {
                                    post_phase_tid = tid;
                                }''',
        '''// A9TAS_GENERATED_BEGIN anchor=c9c_barrel_layout
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                                if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                    cycle_tid != 0 &&
#endif
                                    report.read_errors == 0 &&
                                    report.semantic_errors == 0 &&
                                    !barrel_runtime.OnCertifiedC9C(
                                        static_cast<std::uint32_t>(tid),
                                        static_cast<std::uint32_t>(
                                            machine.frame_index))) {
                                    barrel_process_fatal =
                                        barrel_runtime.requires_process_stop();
                                    semantic_fault("barrel_c9c_begin_failed",
                                                   tid, dr6);
                                }
#endif
                                if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                    cycle_tid != 0 &&
#endif
                                    report.read_errors == 0 &&
                                    report.semantic_errors == 0 &&
                                    !ProgramStoppedThread(
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                                         tid,
                                         barrel_runtime.watch_layout().dr0_rbx_first,
                                         barrel_runtime.watch_layout().dr1_angular_aux,
                                         barrel_runtime.watch_layout().dr2_f64,
                                         barrel_runtime.watch_layout().dr3_world_commit,
                                         barrel_runtime.watch_layout().parallel_active_dr7
#else
                                         tid, completion, callback_flags, f64,
                                         world_accumulator, PipelineDr7()
#endif
                                         )) {
                                    ++report.ptrace_errors;
                                } else if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                           cycle_tid != 0 &&
#endif
                                           report.read_errors == 0 &&
                                           report.semantic_errors == 0) {
                                    post_phase_tid = tid;
                                }
// A9TAS_GENERATED_END anchor=c9c_barrel_layout''',
    ),
    (
        "post_phase_dr0",
        '''                } else if (hit == 1UL) {
                    // The completion transition must already have happened''',
        '''// A9TAS_GENERATED_BEGIN anchor=post_phase_dr0
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                } else if (machine.stage == Stage::kWaitF64 && hit == 1UL) {
                    const bool was_waiting_second =
                        barrel_runtime.waiting_rbx_second();
                    const bool window_was_already_frozen =
                        !barrel_window_frozen_tids.empty();
                    const bool freeze_ok =
                        !was_waiting_second ||
                        freeze_barrel_window_others(tid);
                    const bool handled = freeze_ok && (was_waiting_second
                        ? barrel_runtime.OnRbxSecondStore(
                              static_cast<std::uint32_t>(tid))
                        : barrel_runtime.OnRbxFirstStore(
                              static_cast<std::uint32_t>(tid)));
                    if (!handled) {
                        barrel_process_fatal =
                            !freeze_ok ||
                            barrel_runtime.requires_process_stop();
                        semantic_fault("barrel_rbx_store_failed", tid, dr6);
                    } else {
                        if (was_waiting_second &&
                            !window_was_already_frozen &&
                            !resume_barrel_window_others()) {
                            barrel_process_fatal = true;
                            semantic_fault(
                                "barrel_rbx_resume_failed", tid, dr6);
                        }
                        const auto& layout = barrel_runtime.watch_layout();
                        const std::uintptr_t next_rbx = was_waiting_second
                            ? layout.dr0_rbx_first : layout.dr0_rbx_second;
                        if (!ProgramStoppedThread(
                                tid, next_rbx, layout.dr1_angular_aux,
                                layout.dr2_f64, layout.dr3_world_commit,
                                layout.parallel_active_dr7))
                            ++report.ptrace_errors;
                    }
                } else if (machine.stage == Stage::kWaitF64 && hit == 2UL) {
                    if (!freeze_barrel_window_others(tid)) {
                        barrel_process_fatal = true;
                        semantic_fault("barrel_yaw_freeze_failed", tid, dr6);
                    } else if (!barrel_runtime.OnAngularAux(
                            static_cast<std::uint32_t>(tid))) {
                        barrel_process_fatal =
                            barrel_runtime.requires_process_stop();
                        semantic_fault("barrel_yaw_aux_failed", tid, dr6);
                    }
#endif
                } else if (hit == 1UL) {
                    // The completion transition must already have happened
// A9TAS_GENERATED_END anchor=post_phase_dr0''',
    ),
    (
        "f64_barrel_terminal",
        '''                } else if (hit == 4UL) {
                    std::uint32_t bits = 0;
                    float value = 0.0f;
                    std::uint32_t actions = 0;
                    if (!ReadExact(mem, f64, &bits, sizeof(bits))) {
                        ++report.read_errors;
                    } else {
                        std::memcpy(&value, &bits, sizeof(value));
                        if (!std::isfinite(value) ||
                            std::fabs(value) > 1000000.0f ||
                            !Advance(&machine, Event::kF64, false, false,
                                     false, &actions))
                            semantic_fault("unexpected_f64", tid, dr6);
                        else
                            pending.f64_event = report.event_count;
                    }
                }''',
        '''// A9TAS_GENERATED_BEGIN anchor=f64_barrel_terminal
                } else if (hit == 4UL) {
                    std::uint32_t bits = 0;
                    float value = 0.0f;
                    std::uint32_t actions = 0;
                    std::uint16_t flags_value = 0;
                    if (!ReadExact(mem, f64, &bits, sizeof(bits)) ||
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                        !ReadExact(mem, callback_flags, &flags_value,
                                   sizeof(flags_value)) ||
#endif
                        false) {
                        ++report.read_errors;
                    } else {
                        std::memcpy(&value, &bits, sizeof(value));
                        bool barrel_ok = true;
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                        barrel_ok = barrel_runtime.OnF64(
                            static_cast<std::uint32_t>(tid), flags_value,
                            machine.frame_index + 1 == frames.size());
                        if (!barrel_ok)
                            barrel_process_fatal =
                                barrel_runtime.requires_process_stop();
#endif
                        if (!std::isfinite(value) ||
                            std::fabs(value) > 1000000.0f || !barrel_ok ||
                            !Advance(&machine, Event::kF64, false, false,
                                     false, &actions)) {
                            semantic_fault("unexpected_f64", tid, dr6);
                        } else {
                            pending.f64_event = report.event_count;
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                            const auto& layout = barrel_runtime.watch_layout();
                            if (!ProgramStoppedThread(
                                    tid, 0, layout.dr1_callback_flags, 0,
                                    layout.dr3_world_commit,
                                    layout.post_f64_dr7))
                                ++report.ptrace_errors;
#endif
                        }
                    }
                }
// A9TAS_GENERATED_END anchor=f64_barrel_terminal
''',
    ),
    (
        "barrel_deferred_release",
        '''                            } else {
                                pending.callback_deferred_clear_event =
                                    report.event_count;
                            }
                        } else {
                            semantic_fault("unexpected_callback", tid, dr6);
                        }''',
        '''// A9TAS_GENERATED_BEGIN anchor=barrel_deferred_release
                            } else {
                                pending.callback_deferred_clear_event =
                                    report.event_count;
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                                if (!resume_barrel_window_others()) {
                                    barrel_process_fatal = true;
                                    semantic_fault(
                                        "barrel_yaw_resume_failed", tid,
                                        dr6);
                                }
#endif
                            }
                        } else {
                            semantic_fault("unexpected_callback", tid, dr6);
                        }
// A9TAS_GENERATED_END anchor=barrel_deferred_release''',
    ),
    (
        "world_barrel_commit",
        '''                        !pending_audit || tid == cycle_tid ||
                        pending.callback_deferred_clear_event == 0 ||
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                        !a9tas::final_writer_unified_v1::''',
        '''// A9TAS_GENERATED_BEGIN anchor=world_barrel_commit
                        !pending_audit || tid == cycle_tid ||
                        pending.callback_deferred_clear_event == 0 ||
#ifdef A9TAS_BARREL_SUCCESSOR_V1
                        !barrel_runtime.OnWorldCommit(
                            static_cast<std::uint32_t>(machine.frame_index),
                            pending.callback_close_event != 0,
                            pending.callback_deferred_clear_event != 0) ||
#endif
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                        !a9tas::final_writer_unified_v1::
// A9TAS_GENERATED_END anchor=world_barrel_commit''',
    ),
    (
        "barrel_cleanup",
        '''            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            semantic_fault("unexpected_stop", tid, dr6);
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }''',
        '''// A9TAS_GENERATED_BEGIN anchor=barrel_cleanup
#ifdef A9TAS_BARREL_SUCCESSOR_V1
            if (barrel_process_fatal ||
                barrel_runtime.requires_process_stop()) {
                barrel_process_fatal = true;
                // The target is the explicitly scoped game process.  Never
                // resume a mutation-uncertain transient payload window.
                kill(pid, SIGKILL);
            } else
#endif
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            semantic_fault("unexpected_stop", tid, dr6);
#ifdef A9TAS_BARREL_SUCCESSOR_V1
            if (signal == SIGTRAP) {
                // A payload BRK is deliberately not a HWBP event.  Swallowing
                // it would resume after a fail-closed violation.
                barrel_process_fatal = true;
                kill(pid, SIGKILL);
            } else
#endif
            {
                const int deliver = signal == SIGTRAP ? 0 : signal;
                if (!ContinueThread(tid, deliver))
                    ++report.ptrace_errors;
                else if (tracked)
                    tracked->stopped = false;
            }
        }
// A9TAS_GENERATED_END anchor=barrel_cleanup''',
    ),
    (
        "barrel_success_condition",
        '''#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    success = success && final_writer_final_ok &&
              final_writer_payload_report_ok;
#endif''',
        '''// A9TAS_GENERATED_BEGIN anchor=barrel_success_condition
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    success = success && final_writer_final_ok &&
              final_writer_payload_report_ok;
#endif
#ifdef A9TAS_BARREL_SUCCESSOR_V1
    success = success && barrel_runtime.Success(frames.size()) &&
              !barrel_process_fatal;
#endif
// A9TAS_GENERATED_END anchor=barrel_success_condition''',
    ),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    document = {
        "schema": "a9tas-barrel-successor-replacements-v1",
        "complete_successor": True,
        "required_anchor_ids": [item[0] for item in ANCHORS],
        "replacements": [
            {"id": anchor, "old_utf8": old, "new_utf8": new}
            for anchor, old, new in ANCHORS
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"BARREL_SUCCESSOR_PLAN_BUILDER passed=1 anchors={len(ANCHORS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
