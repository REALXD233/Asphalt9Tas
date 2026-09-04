#!/usr/bin/env python3
"""Aggregate explicitly-allowlisted offline policy/semantic tests.

This runner uses a fixed allowlist; it never auto-discovers `test_*.py` because
some parsers require live device files as arguments.  It runs every allowlisted
test, records its command, exit code, wall-clock duration and PASS/FAIL label,
continues after failures, and returns non-zero only if at least one test
failed.

Two kinds of entries are supported:
- ``unittest``: modules that use :mod:`unittest` and expose ``unittest.main()``.
  These are loaded in-process; a module that loads zero tests is a hard FAIL.
- ``script``: modules that expose ``def main()`` and are invoked as
  ``python <module>.py`` via :mod:`subprocess`.  Exit code 0 is PASS, anything
  else is FAIL.

Hard guarantees:
- no ADB, `su -c`, emulator, IDA, network, or any `run-*.ps1` live mode
- no `PrepareFreshProcess`, `ProbePreparedProcess`, or any other live runner
  mode is invoked
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import subprocess
import sys
import time
import unittest
from pathlib import Path
from typing import Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
TOOLS_DIR = SCRIPT_DIR
WORKSPACE_ROOT = SCRIPT_DIR.parent.parent

# Ensure tools/ is importable for in-process unittest loaders.
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

PY = sys.executable or "python"

GROUP_ORIGINAL_SEMANTICS = "original_action_semantics"
GROUP_SOURCE_PARITY = "source_parity"
GROUP_FC0 = "fc0_frame_callback_bootstrap"
GROUP_FC1_RESOLVER = "fc1_payload_elf_resolver"
GROUP_FC1_TRANSACTION = "fc1_frame_callback_transaction"
GROUP_FC1_RUNNER = "fc1_guarded_runner_report_validator"
GROUP_FC1_REPORT_VALIDATOR = "fc1_report_validator"
GROUP_FC2_PAYLOAD = "fc2_deferred_registration_payload"
GROUP_FC3_PROTOCOL = "fc3_replay_observer_protocol"
GROUP_FC3_IDENTITY = "fc3_replay_observer_identity"
GROUP_FC3_EVENT_CORE = "fc3_replay_observer_event_core"
GROUP_FC3_SUCCESSOR = "fc3_successor_controller"
GROUP_UNIFIED_TICK = "unified_tick_executor"
GROUP_SAME_BYTES = "same_bytes_executor"
GROUP_CONDITIONAL = "conditional_executor"
GROUP_PIPELINE = "pipeline_order"
GROUP_WORKER_BOUNDARY = "worker_boundary"
GROUP_ZERO_GAP = "zero_gap"
GROUP_NATIVE_PHYSICS = "native_physics_recording"
GROUP_NITRO = "nitro_thread_affinity"
GROUP_NATURAL_PREROLL = "natural_preroll"
GROUP_NATURAL_ACTION = "natural_action"
GROUP_TRANSFORM_CORRECTION = "transform_correction"
GROUP_VEHICLE_STATE = "vehicle_state"
GROUP_PARITY_EVIDENCE_INDEX = "evidence_index"
GROUP_ARTIFACT_TOOLING = "artifact_tooling"

UT = "unittest"
SC = "script"

# Each entry: (module_name, friendly_label, group, kind)
ALLOWLIST: list[tuple[str, str, str, str]] = [
    ("test_original_action_semantics_v1", "original action semantics", GROUP_ORIGINAL_SEMANTICS, UT),
    ("test_original_source_action_parity_v1", "upstream source parity", GROUP_SOURCE_PARITY, UT),
    ("test_lifecycle_barrel_source_policy_v1", "review-only lifecycle barrel field capture", GROUP_SOURCE_PARITY, SC),
    ("test_lifecycle_natural_action_barrel_recording_v1", "strict lifecycle natural-action barrel recording", GROUP_SOURCE_PARITY, UT),
    ("test_camera_replay_protocol_policy_v1", "lossless upstream camera replay sidecar", GROUP_SOURCE_PARITY, SC),
    ("verify_camera_static_candidate_v1", "pinned Android camera static route", GROUP_SOURCE_PARITY, SC),
    ("test_camera_active_state_resolver_policy_v1", "read-only active blended-camera resolver", GROUP_SOURCE_PARITY, SC),
    ("test_camera_active_state_check_policy_v1", "read-only active-camera live diagnostic policy", GROUP_SOURCE_PARITY, SC),
    ("test_camera_wrapper_graph_policy_v1", "bounded read-only blended-camera wrapper graph", GROUP_SOURCE_PARITY, SC),
    ("test_camera_manager_graph_policy_v1", "bounded read-only camera-manager graph", GROUP_SOURCE_PARITY, SC),
    ("test_camera_shape_state_policy_v1", "bounded read-only camera transform-object sampler", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_state_policy_v1", "bounded read-only RaceView manager-shape consistency sampler", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_phase_policy_v1", "timestamped read-only RaceView manager-shape phase sampler", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_phase_analysis_v1", "offline RaceView manager-shape lag analysis", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_callback_node_policy_v1", "exact read-only RaceView callback-node resolver", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_record_payload_policy_v1", "build-only post-original RaceView sidecar recorder", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_record_transaction_policy_v1", "single-slot RaceView recorder install and restore transaction", GROUP_SOURCE_PARITY, SC),
    ("test_run_camera_raceview_record_policy_v1", "guarded one-shot RaceView record-only live runner", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_same_state_payload_policy_v1", "five-callback post-original RaceView same-state writer", GROUP_SOURCE_PARITY, SC),
    ("test_camera_raceview_same_state_transaction_policy_v1", "single-slot RaceView same-state install and restore transaction", GROUP_SOURCE_PARITY, SC),
    ("test_run_camera_raceview_same_state_policy_v1", "guarded one-shot RaceView same-state live runner", GROUP_SOURCE_PARITY, SC),
    ("validate_camera_raceview_same_state_report_v1", "strict finalized RaceView same-state receipt validator", GROUP_SOURCE_PARITY, SC),
    ("test_upstream_parity_matrix_v2", "corrected Android parity matrix", GROUP_SOURCE_PARITY, UT),
    ("test_upstream_camera_playback_semantics_v1", "upstream TAS excludes camera packets from normal playback", GROUP_SOURCE_PARITY, UT),
    ("test_camera_tool_semantics_v1", "standalone upstream Camera Tool semantics", GROUP_SOURCE_PARITY, UT),
    ("test_camera_tool_payload_policy_v1", "standalone Android Camera Tool payload", GROUP_SOURCE_PARITY, SC),
    ("test_camera_tool_transaction_policy_v1", "Camera Tool single-slot transaction", GROUP_SOURCE_PARITY, SC),
    ("test_run_camera_tool_policy_v1", "guarded standalone Camera Tool runner", GROUP_SOURCE_PARITY, SC),
    ("test_camera_tool_stream_policy_v1", "persistent payload-only Camera Tool command stream", GROUP_SOURCE_PARITY, SC),
    ("test_camera_tool_controller_math_v1", "upstream Free Flight and Orbital controller math", GROUP_SOURCE_PARITY, SC),
    ("test_run_camera_tool_free_flight_policy_v1", "persistent Free Flight host controller", GROUP_SOURCE_PARITY, SC),
    ("test_run_camera_tool_orbital_policy_v1", "persistent authoritative-target Orbital controller", GROUP_SOURCE_PARITY, SC),
    ("test_camera_tool_runtime_core_v2_policy", "phase-aligned in-callback Camera Tool runtime core", GROUP_SOURCE_PARITY, SC),
    ("test_camera_tool_runtime_payload_v2_policy", "phase-aligned ARM64 Camera Tool runtime payload", GROUP_SOURCE_PARITY, SC),
    ("test_camera_tool_runtime_transport_v2_policy", "phase-aligned Camera Tool install and input transport", GROUP_SOURCE_PARITY, SC),
    ("test_run_camera_tool_runtime_v2_policy", "phase-aligned Camera Tool v2 host workflow", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_tick_state_machine_v1", "authoritative tick state machine", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_tick_cpp_policy_v1", "authoritative tick C++ core policy", GROUP_SOURCE_PARITY, SC),
    ("test_barrel_stabilization_replay_core_policy_v1", "source-bound post-original barrel stabilization replay core", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_yaw_tail_payload_policy_v1", "build-only late-boundary BarrelYaw shadow-vptr payload", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_yaw_tail_host_transaction_policy_v1", "hash-pinned publish-last BarrelYaw host transaction", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_yaw_tail_resolver_mapping_model_v1", "host-executed NativeBridge payload mapping model", GROUP_ORIGINAL_SEMANTICS, UT),
    ("test_barrel_yaw_tail_transport_policy_v1", "publish-last BarrelYaw payload transport and restore", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_rbx_two_store_transaction_policy_v1", "source-bound BarrelRBX two-store certificate", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_post_c9c_slot_schedule_policy_v1", "four-slot post-C9C barrel watchpoint schedule", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_post_c9c_watch_layout_policy_v1", "exact post-C9C DR0 RBX-to-angular swap layout", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_successor_preload_policy_v1", "hash-pinned four-payload barrel successor preload", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_parse_successor_classifier_v1", "strict read-only successor phase and stack classifier", GROUP_ORIGINAL_SEMANTICS, UT),
    ("test_successor_classifier_source_policy_v1", "signal-transparent read-only successor classifier source policy", GROUP_ORIGINAL_SEMANTICS, UT),
    ("test_generate_barrel_successor_source_v1", "exact-hash complete successor source generator", GROUP_ORIGINAL_SEMANTICS, UT),
    ("test_barrel_successor_source_generator_policy_v1", "fail-closed successor generator policy", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_barrel_successor_executor_policy_v1", "compiled successor overlay integration policy", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_run_barrel_successor_replay_policy_v1", "guarded fresh-process successor runner policy", GROUP_ORIGINAL_SEMANTICS, SC),
    ("test_upstream_replay_control_plane_v1", "upstream replay control plane", GROUP_SOURCE_PARITY, UT),
    ("test_controller_shadow_m1_executor_policy_v1", "M1 delta-only executor policy", GROUP_SOURCE_PARITY, SC),
    ("test_controller_shadow_phase_paced_executor_policy_v1", "controller-shadow proven-boundary phase-paced executor policy", GROUP_SOURCE_PARITY, SC),
    ("verify_known_good_900_exact_interval_v2", "user-validated live-proven 900-frame exact-interval baseline", GROUP_SOURCE_PARITY, SC),
    ("verify_phase_paced_five_frame_candidate_v1", "pinned A9PHEX1 five-frame candidate", GROUP_SOURCE_PARITY, SC),
    ("test_controller_shadow_m1_preflight_policy_v1", "M1 staged read-only preflight policy", GROUP_SOURCE_PARITY, SC),
    ("test_gameplay_input_controller_mapping_adapter_v1", "LDPlayer-sized controller object-mapping adapter", GROUP_SOURCE_PARITY, UT),
    ("test_m1_three_payload_preload_policy_v1", "M1 three-payload preload policy", GROUP_SOURCE_PARITY, SC),
    ("test_parse_m1_executor_report_v1", "strict M1 executor report parser", GROUP_SOURCE_PARITY, UT),
    ("test_parse_phase_paced_executor_report_v1", "strict proven-boundary phase-paced executor report parser", GROUP_SOURCE_PARITY, UT),
    ("test_run_controller_shadow_m1_gate_policy_v1", "guarded M1 host runner policy", GROUP_SOURCE_PARITY, SC),
    ("test_run_controller_shadow_phase_paced_gate_policy_v1", "independent guarded A9PHEX1 host runner policy", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_replay_session_v1", "authoritative replay session semantics", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_replay_session_cpp_policy_v1", "authoritative replay session C++ policy", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_unified_adapter_v1", "authoritative unified adapter semantics", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_unified_adapter_cpp_policy_v1", "authoritative unified adapter C++ policy", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_neutral_observer_v1", "authoritative five-frame neutral observer", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_neutral_observer_cpp_policy_v1", "authoritative neutral observer C++ policy", GROUP_SOURCE_PARITY, SC),
    ("validate_authoritative_neutral_observer_v1", "A9ANO1 report validator selftest", GROUP_SOURCE_PARITY, SC),
    ("test_run_authoritative_neutral_observer_policy_v1", "guarded A9ANO1 runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_fixed_delta_v1", "authoritative fixed-delta semantics", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_fixed_delta_cpp_policy_v1", "authoritative fixed-delta C++ policy", GROUP_SOURCE_PARITY, SC),
    ("validate_authoritative_fixed_delta_v1", "A9AFD1 report validator selftest", GROUP_SOURCE_PARITY, SC),
    ("test_run_authoritative_fixed_delta_policy_v1", "guarded A9AFD1 runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_make_authoritative_steering_projection_v1", "authoritative steering source projection", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_steering_transport_v1", "authoritative steering pair semantics", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_steering_transport_cpp_policy_v1", "authoritative steering C++ transport policy", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_steering_integration_v1", "authoritative steering end-to-end model", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_adapter_bridge_cpp_policy_v1", "authoritative adapter bridge C++ policy", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_natural_handoff_v1", "authoritative natural handoff semantics", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_natural_handoff_cpp_policy_v1", "authoritative natural handoff C++ policy", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_steering_executor_v1", "authoritative steering executor semantics", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_steering_cpp_policy_v1", "authoritative steering C++ policy", GROUP_SOURCE_PARITY, SC),
    ("validate_authoritative_steering_v1", "A9AST1 report validator selftest", GROUP_SOURCE_PARITY, SC),
    ("test_run_authoritative_steering_policy_v1", "guarded A9AST1 runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_steering_search_only_policy_v1", "compile-time zero-write steering search policy", GROUP_SOURCE_PARITY, SC),
    ("validate_authoritative_steering_search_only_v1", "zero-write steering search report validator", GROUP_SOURCE_PARITY, SC),
    ("test_authoritative_steering_search_runner_policy_v1", "guarded zero-write steering search runner", GROUP_SOURCE_PARITY, UT),
    ("test_startline_pause_gate_v1", "start-line pause-edge gate semantics", GROUP_SOURCE_PARITY, UT),
    ("test_startline_prearm_protocol_v1", "start-line no-attach prearm protocol", GROUP_SOURCE_PARITY, UT),
    ("test_startline_tick_recorder_policy_v1", "start-line tick-zero recorder policy", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_steering_startline_policy_v1", "direct start-line steering replay policy", GROUP_SOURCE_PARITY, UT),
    ("test_startline_runners_policy_v1", "paired guarded start-line runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_controls_transport_v1", "authoritative steering and brake pair transport", GROUP_SOURCE_PARITY, UT),
    ("test_authoritative_controls_startline_v1", "direct start-line steering and brake replay", GROUP_SOURCE_PARITY, UT),
    ("validate_authoritative_controls_startline_v1", "start-line controls report validator selftest", GROUP_SOURCE_PARITY, SC),
    ("test_make_authoritative_controls_projection_v1", "authoritative controls source projection", GROUP_SOURCE_PARITY, UT),
    ("test_startline_unified_replay_policy_v1", "start-line unified controls and physics replay policy", GROUP_SOURCE_PARITY, UT),
    ("test_final_writer_replay_policy_v1", "finite natural final-writer replay payload policy", GROUP_TRANSFORM_CORRECTION, SC),
    ("test_final_writer_replay_elf_resolver_v1", "hash-pinned final-writer payload ELF resolver", GROUP_TRANSFORM_CORRECTION, SC),
    ("test_final_writer_target_blob_v1", "strict A9UTK1 final-writer target projection", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_make_final_writer_prefix_v1", "strict 30/360/900-frame final-writer gate prefixes", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_parse_unified_executor_report_v8", "A9UER8 final-writer unified report parser", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_validate_final_writer_report_pair_v1", "A9UER8/A9FWR1 per-frame provenance validator", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_compare_final_writer_trajectory_runs_v1", "multi-run pre-correction vehicle trajectory equivalence", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_validate_action_control_replay_v1", "A9UTK1 to A9UER6/A9UER8 exact steering-brake replay binding", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_final_writer_cursor_binding_v1", "fixed-delta to final-writer cursor binding", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_final_writer_transaction_core_v1", "final-writer finite transaction preparation and rollback", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_final_writer_storage_transaction_v1", "final-writer publish-last payload storage transaction", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_final_writer_unified_integration_policy_v1", "guarded final-writer unified executor integration", GROUP_TRANSFORM_CORRECTION, SC),
    ("validate_final_writer_payload_report_v1", "A9FWR1 final-writer payload report validator", GROUP_TRANSFORM_CORRECTION, SC),
    ("test_final_writer_live_gate_policy_v1", "final-writer preload and one-shot live-gate policy", GROUP_TRANSFORM_CORRECTION, SC),
    ("test_run_final_writer_gate_policy_v1", "final-writer guarded host runner policy", GROUP_TRANSFORM_CORRECTION, SC),
    ("test_final_writer_upstream_parity_v1", "AluTasV2 final-writer source-order parity", GROUP_ORIGINAL_SEMANTICS, UT),
    ("test_replay_smoothness_analysis_v1", "replay correction-cluster smoothness analysis", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_race_lifecycle_static_v1", "hash-pinned Android race lifecycle resolver", GROUP_SOURCE_PARITY, UT),
    ("test_race_lifecycle_object_resolver_policy_v1", "read-only Android race object resolver policy", GROUP_SOURCE_PARITY, SC),
    ("test_run_race_lifecycle_object_policy_v1", "guarded read-only race object runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_race_lifecycle_transition_policy_v1", "one-shot authoritative race-begin transition observer policy", GROUP_SOURCE_PARITY, SC),
    ("test_run_race_lifecycle_transition_policy_v1", "guarded authoritative race-begin host runner policy", GROUP_SOURCE_PARITY, SC),
    ("test_lifecycle_final_writer_policy_v1", "lifecycle-bound final-writer replay handoff policy", GROUP_SOURCE_PARITY, SC),
    ("test_run_lifecycle_final_writer_policy_v1", "guarded lifecycle-bound final-writer host runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_lifecycle_source_policy_v1", "lifecycle-bound synchronized source capture policy", GROUP_SOURCE_PARITY, SC),
    ("test_lifecycle_source_recording_v1", "A9USR5 lifecycle source report envelope and recording cross-bind", GROUP_SOURCE_PARITY, SC),
    ("test_lifecycle_steer_drift_recording_v1", "A9USR5 lifecycle steer/drift action-content proof", GROUP_SOURCE_PARITY, UT),
    ("test_run_lifecycle_source_policy_v1", "guarded lifecycle-bound source host runner policy", GROUP_SOURCE_PARITY, UT),
    ("test_prepare_lifecycle_replay_artifacts_policy_v1", "lifecycle source-to-replay artifact bridge policy", GROUP_SOURCE_PARITY, UT),
    ("test_fc3_phase_role_policy_v1", "FC-3 phase-role correction", GROUP_SOURCE_PARITY, UT),
    ("test_frame_callback_bootstrap_policy_v1", "FC-0 frame callback bootstrap policy", GROUP_FC0, SC),
    ("test_fc1_payload_elf_resolver_v1", "FC-1 payload ELF resolver", GROUP_FC1_RESOLVER, SC),
    ("test_fc1_frame_callback_transaction_policy_v1", "FC-1 frame callback transaction policy", GROUP_FC1_TRANSACTION, SC),
    ("test_run_fc1_frame_callback_policy_v1", "FC-1 guarded runner policy", GROUP_FC1_RUNNER, UT),
    ("validate_fc1_report_v1", "FC-1 report validator selftest", GROUP_FC1_REPORT_VALIDATOR, SC),
    ("test_frame_callback_deferred_registration_policy_v1", "FC-2 deferred registration payload policy", GROUP_FC2_PAYLOAD, SC),
    ("test_fc2_payload_elf_resolver_v1", "FC-2 payload ELF resolver", GROUP_FC2_PAYLOAD, SC),
    ("validate_fc2_report_v1", "FC-2 report validator selftest", GROUP_FC2_PAYLOAD, SC),
    ("test_fc2_frame_callback_transaction_policy_v1", "FC-2 three-frame transaction policy", GROUP_FC2_PAYLOAD, SC),
    ("test_run_fc2_frame_callback_policy_v1", "FC-2 guarded runner policy", GROUP_FC2_PAYLOAD, UT),
    ("audit_fc2_live_candidate_v1", "FC-2 cross-artifact live-candidate audit", GROUP_FC2_PAYLOAD, SC),
    ("test_fc3_replay_observer_protocol_policy_v1", "FC-3 replay-observer protocol policy", GROUP_FC3_PROTOCOL, SC),
    ("test_fc3_identity_resolver_policy_v1", "FC-3 read-only identity resolver policy", GROUP_FC3_IDENTITY, SC),
    ("test_fc3_observer_event_core_policy_v1", "FC-3 four-watchpoint event-core policy", GROUP_FC3_EVENT_CORE, SC),
    ("test_fc3_successor_controller_policy_v1", "FC-2 to FC-3 successor-controller policy", GROUP_FC3_SUCCESSOR, SC),
    ("test_fc3_successor_cleanup_model_v1", "FC-3 successor fail-closed cleanup model", GROUP_FC3_SUCCESSOR, UT),
    ("validate_fc3_report_v1", "FC-3 report validator selftest", GROUP_FC3_SUCCESSOR, SC),
    ("audit_fc3_successor_candidate_v1", "FC-3 successor cross-artifact closure audit", GROUP_FC3_SUCCESSOR, SC),
    ("validate_fc3_phase_map_v1", "FC-3 phase-map report validator selftest", GROUP_FC3_SUCCESSOR, SC),
    ("validate_fc3_entry_stability_v1", "FC-3 entry-stability report validator selftest", GROUP_FC3_SUCCESSOR, SC),
    ("test_run_fc3_phase_map_policy_v1", "FC-3 phase-map guarded runner policy", GROUP_FC3_SUCCESSOR, UT),
    ("test_run_fc3_entry_stability_policy_v1", "FC-3 entry-stability guarded runner policy", GROUP_FC3_SUCCESSOR, UT),
    ("test_unified_tick_executor_semantics_v1", "unified tick executor semantics", GROUP_UNIFIED_TICK, UT),
    ("test_unified_executor_build_policy_v1", "unified executor build policy", GROUP_UNIFIED_TICK, UT),
    ("test_parse_unified_executor_report_v1", "parse A9UER1 report v1", GROUP_UNIFIED_TICK, UT),
    ("test_parse_unified_executor_report_v2", "parse A9UER1 report v2", GROUP_UNIFIED_TICK, UT),
    ("test_same_bytes_transport_semantics_v1", "same-bytes transport semantics", GROUP_SAME_BYTES, UT),
    ("test_parse_same_bytes_audit_v1", "parse A9SBT1 audit", GROUP_SAME_BYTES, UT),
    ("test_conditional_transport_semantics_v1", "conditional transport semantics", GROUP_CONDITIONAL, UT),
    ("test_parse_conditional_audit_v1", "parse A9CDT1 audit", GROUP_CONDITIONAL, UT),
    ("test_conditional_runner_policy_v1", "conditional runner policy", GROUP_CONDITIONAL, UT),
    ("test_parse_hwbp_pipeline_order_v1", "parse HWBP pipeline order", GROUP_PIPELINE, UT),
    ("test_parse_hwbp_worker_boundary_v1", "parse HWBP worker boundary", GROUP_WORKER_BOUNDARY, UT),
    ("test_parse_hwbp_zero_gap_v1", "parse HWBP zero gap", GROUP_ZERO_GAP, UT),
    ("test_parse_hwbp_worker_stack_scope_v1", "parse HWBP worker stack scope", GROUP_WORKER_BOUNDARY, UT),
    ("test_parse_hwbp_executor_stack_affinity_v1", "parse HWBP executor stack affinity", GROUP_WORKER_BOUNDARY, UT),
    ("test_parse_hwbp_barrel_angular_v1", "parse HWBP barrel angular", GROUP_VEHICLE_STATE, UT),
    ("test_parse_physics_executor_affinity_v1", "parse physics executor affinity", GROUP_VEHICLE_STATE, UT),
    ("test_parse_game_action_submission_affinity_v1", "parse game action submission affinity", GROUP_VEHICLE_STATE, UT),
    ("test_native_physics_recording_v1", "native physics recording", GROUP_NATIVE_PHYSICS, UT),
    ("test_unified_tick_recording_v1", "unified tick recording", GROUP_UNIFIED_TICK, UT),
    ("test_synchronized_tick_recording_v1", "synchronized tick recording", GROUP_NATIVE_PHYSICS, UT),
    ("test_synchronized_action_until_release_recording_v1", "synchronized action until release recording", GROUP_NATURAL_ACTION, UT),
    ("test_synchronized_action_window_recording_v1", "synchronized action window recording", GROUP_NATURAL_ACTION, UT),
    ("test_synchronized_brake_recording_v1", "synchronized brake recording", GROUP_NATURAL_PREROLL, UT),
    ("test_aligned_tick_replay_v1", "aligned tick replay", GROUP_NATURAL_PREROLL, UT),
    ("test_aligned_tick_replay_policy_v1", "aligned tick replay policy", GROUP_NATURAL_PREROLL, UT),
    ("test_aligned_brake_replay_v1", "aligned brake replay", GROUP_NATURAL_PREROLL, UT),
    ("test_aligned_natural_replay_runner_policy_v1", "aligned natural replay runner policy", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_anchor_v1", "natural preroll anchor", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_search_report_v1", "natural preroll search report", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_recorder_policy_v1", "natural preroll recorder policy", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_replay_policy_v1", "natural preroll replay policy", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_replay_runner_policy_v1", "natural preroll replay runner policy", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_source_runner_policy_v1", "natural preroll source runner policy", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_preroll_brake_runner_policy_v1", "natural preroll brake runner policy", GROUP_NATURAL_PREROLL, UT),
    ("test_natural_action_window_runner_policy_v1", "natural action window runner policy", GROUP_NATURAL_ACTION, UT),
    ("test_natural_action_until_release_runner_policy_v1", "natural action until release runner policy", GROUP_NATURAL_ACTION, UT),
    ("test_paused_anchor_recorder_policy_v1", "paused anchor recorder policy", GROUP_NATURAL_PREROLL, UT),
    ("test_input_cycle_startline_source_policy_v1", "input-cycle startline source policy", GROUP_NATURAL_PREROLL, SC),
    ("test_run_input_cycle_startline_source_policy_v1", "input-cycle startline source runner policy", GROUP_NATURAL_PREROLL, SC),
    ("test_input_cycle_startline_anchor_model_v1", "input-cycle startline anchor model", GROUP_NATURAL_PREROLL, UT),
    ("test_synchronized_tick_recorder_policy_v1", "synchronized tick recorder policy", GROUP_NATIVE_PHYSICS, UT),
    ("test_synchronized_tick_runner_policy_v1", "synchronized tick runner policy", GROUP_NATIVE_PHYSICS, UT),
    ("test_gate5_handoff_runner_policy_v1", "gate5 handoff runner policy", GROUP_CONDITIONAL, UT),
    ("test_verify_gate5_run1_evidence_v1", "verify gate5 run1 evidence", GROUP_CONDITIONAL, UT),
    ("test_verify_gate5_run2_evidence_v1", "verify gate5 run2 evidence", GROUP_CONDITIONAL, UT),
    ("test_gate6_runner_policy_v1", "gate6 runner policy", GROUP_UNIFIED_TICK, UT),
    ("test_verify_gate6_phase_only_result_v1", "verify gate6 phase-only result", GROUP_UNIFIED_TICK, UT),
    ("test_unified_phase_only_gate_v1", "unified phase-only gate", GROUP_UNIFIED_TICK, UT),
    ("test_gate7_runner_policy_v1", "gate7 runner policy", GROUP_UNIFIED_TICK, UT),
    ("test_unified_steering_gate_v1", "unified steering gate", GROUP_UNIFIED_TICK, UT),
    ("test_gate8_runner_policy_v1", "gate8 runner policy", GROUP_UNIFIED_TICK, UT),
    ("test_unified_final_correction_gate_v1", "unified final correction gate", GROUP_UNIFIED_TICK, UT),
    ("test_gate9_runner_policy_v1", "gate9 runner policy", GROUP_UNIFIED_TICK, UT),
    ("test_unified_steering_final_gate_v1", "unified steering final gate", GROUP_UNIFIED_TICK, UT),
    ("test_gate10_brake_runner_policy_v1", "gate10 brake runner policy", GROUP_NATURAL_PREROLL, UT),
    ("test_unified_brake_gate_v1", "unified brake gate", GROUP_NATURAL_PREROLL, UT),
    ("test_unified_nitro_observe_gate_v1", "unified nitro observe gate", GROUP_NITRO, UT),
    ("test_nitro_prephysics_mailbox_v1", "nitro prephysics mailbox", GROUP_NITRO, UT),
    ("test_natural_action_callback_mailbox_v1", "natural callback game-action mailbox", GROUP_NITRO, UT),
    ("test_natural_action_callback_mailbox_policy_v1", "natural callback action mailbox safety policy", GROUP_NITRO, UT),
    ("test_natural_action_replay_transport_v1", "AluTasV2 per-frame natural Nitro transport", GROUP_NITRO, UT),
    ("test_natural_action_replay_transport_policy_v1", "per-frame natural Nitro transport C++ policy", GROUP_NITRO, SC),
    ("test_natural_action_replay_cursor_v1", "natural Nitro and final-writer dual cursor", GROUP_NITRO, UT),
    ("test_natural_action_replay_cursor_policy_v1", "natural Nitro dual cursor C++ policy", GROUP_NITRO, SC),
    ("test_natural_action_replay_host_v1", "natural Nitro external mailbox publication", GROUP_NITRO, UT),
    ("test_natural_action_replay_host_policy_v1", "natural Nitro host publication ordering policy", GROUP_NITRO, SC),
    ("test_final_writer_natural_action_binding_v1", "final-writer and natural Nitro composed cursor", GROUP_NITRO, UT),
    ("test_final_writer_natural_action_binding_policy_v1", "final-writer and natural Nitro binding policy", GROUP_NITRO, SC),
    ("test_final_writer_natural_action_runtime_v1", "verified mailbox and final-writer callback-close runtime", GROUP_NITRO, UT),
    ("test_final_writer_natural_action_runtime_policy_v1", "verified mailbox runtime C++ policy", GROUP_NITRO, SC),
    ("test_make_final_writer_natural_action_gate_v1", "hash-bound 360-frame composite natural-action Gate fixture", GROUP_NITRO, UT),
    ("test_lifecycle_final_writer_natural_action_policy_v1", "lifecycle final-writer natural-action integration policy", GROUP_NITRO, SC),
    ("audit_natural_action_external_replay_controller_v1", "persistent external natural-action controller audit", GROUP_NITRO, SC),
    ("test_final_writer_natural_action_preload_policy_v1", "paired NativeBridge preload policy", GROUP_NITRO, SC),
    ("test_run_final_writer_natural_action_composite_policy_v1", "guarded composite live runner policy", GROUP_NITRO, SC),
    ("test_validate_natural_action_external_replay_report_v1", "dynamic A9NAR6 report validator", GROUP_NITRO, UT),
    ("audit_natural_action_replay_payload_v1", "persistent per-frame natural Nitro payload audit", GROUP_NITRO, SC),
    ("test_natural_action_replay_payload_semantics_v1", "non-blocking per-frame natural Nitro payload semantics", GROUP_NITRO, UT),
    ("test_natural_action_replay_elf_resolver_v1", "hash-pinned persistent Nitro replay payload resolver", GROUP_NITRO, SC),
    ("test_natural_action_replay_sequence_gate_v1", "guarded five-frame natural Nitro sequence Gate", GROUP_NITRO, UT),
    ("test_natural_action_recording_nitro_smoke_policy_v1", "short passive Nitro recording wrapper smoke Gate", GROUP_NITRO, UT),
    ("test_natural_action_host_gate_v1", "A9UTK1 natural action host receipt gate", GROUP_NITRO, UT),
    ("test_authoritative_natural_action_phase_gate_v1", "authoritative natural action phase transaction", GROUP_NITRO, UT),
    ("test_natural_action_callback_consumer_policy_v1", "passive natural action callback consumer policy", GROUP_NITRO, SC),
    ("test_natural_action_callback_lifecycle_v1", "persistent natural action callback lifecycle", GROUP_NITRO, UT),
    ("test_natural_action_callback_lifecycle_policy_v1", "persistent action callback payload policy", GROUP_NITRO, SC),
    ("test_natural_action_lifecycle_elf_resolver_v1", "hash-pinned persistent action payload ELF resolver", GROUP_NITRO, SC),
    ("test_natural_action_lifecycle_transaction_v1", "guarded zero-call lifecycle transaction", GROUP_NITRO, UT),
    ("test_natural_action_lifecycle_transaction_policy_v1", "guarded lifecycle transaction safety policy", GROUP_NITRO, SC),
    ("validate_natural_action_lifecycle_report_v1", "zero-call lifecycle report validator", GROUP_NITRO, SC),
    ("test_natural_action_lifecycle_controller_policy_v1", "build-only zero-call lifecycle controller policy", GROUP_NITRO, SC),
    ("audit_natural_action_lifecycle_candidate_v1", "linked zero-call lifecycle candidate cross-artifact audit", GROUP_NITRO, SC),
    ("audit_natural_action_scheduler_payload_v1", "local-only natural game-scheduler action payload audit", GROUP_NITRO, SC),
    ("test_natural_action_scheduler_elf_resolver_v1", "hash-pinned natural scheduler action payload resolver", GROUP_NITRO, SC),
    ("validate_natural_action_scheduler_report_v1", "one-activation natural scheduler report validator", GROUP_NITRO, SC),
    ("audit_natural_action_scheduler_controller_v1", "unlinked one-activation scheduler controller audit", GROUP_NITRO, SC),
    ("audit_natural_action_scheduler_candidate_v1", "linked one-activation scheduler candidate audit", GROUP_NITRO, SC),
    ("test_run_natural_action_scheduler_policy_v1", "guarded one-activation scheduler runner policy", GROUP_NITRO, SC),
    ("test_run_natural_action_lifecycle_policy_v1", "guarded zero-call lifecycle runner safety policy", GROUP_NITRO, SC),
    ("test_nitro_thread_affinity_observer_v1", "nitro thread affinity observer", GROUP_NITRO, UT),
    ("test_transform_correction_semantics_v1", "transform correction semantics", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_scan_arm64_vslot_calls", "scan ARM64 vslot calls", GROUP_TRANSFORM_CORRECTION, UT),
    ("test_analyze_hwbp_events_v5", "analyze HWBP events v5", GROUP_PIPELINE, UT),
    ("test_make_source_proxy_replay_v1", "make source proxy replay", GROUP_NATURAL_PREROLL, UT),
    ("test_executor_stack_affinity_build_policy_v1", "executor stack affinity build policy", GROUP_WORKER_BOUNDARY, UT),
    ("test_game_action_rpc_build_policy_v1", "game action RPC build policy", GROUP_VEHICLE_STATE, UT),
    ("test_game_action_submission_affinity_build_policy_v1", "game action submission affinity build policy", GROUP_VEHICLE_STATE, UT),
    ("test_game_action_submission_background_policy_v1", "game action submission background policy", GROUP_VEHICLE_STATE, UT),
    ("test_producer_thread_nativebridge_probe_policy_v1", "producer thread NativeBridge probe policy", GROUP_FC1_RUNNER, UT),
    ("test_run_producer_thread_probe_policy_v1", "run producer thread probe policy", GROUP_FC1_RUNNER, UT),
    ("test_producer_action_observe_policy_v1", "producer action observe policy", GROUP_FC1_RUNNER, UT),
    ("test_android_branch_license_v1", "Android portrait branch editor and signed research license", GROUP_ARTIFACT_TOOLING, UT),
    # Batch-3/4 artifact tooling (P4/P3): pure-offline, parameter-free unittest
    # modules only.  Never add modules that need devices, arguments or live
    # artifacts here.
    ("test_inspect_a9_artifact_v1", "A9 artifact inspector", GROUP_ARTIFACT_TOOLING, UT),
    ("test_check_a9_binary_format_registry_v1", "A9 format registry consistency", GROUP_ARTIFACT_TOOLING, UT),
    ("test_inspect_a9_artifact_manifest_v1", "A9 artifact manifest inspector", GROUP_ARTIFACT_TOOLING, UT),
    ("test_a9_artifact_inspector_user_guide_v1", "A9 inspector user guide", GROUP_ARTIFACT_TOOLING, UT),
    ("test_validate_a9_artifact_report_contract_v1", "A9 artifact report contract", GROUP_ARTIFACT_TOOLING, UT),
    ("test_resolve_a9_build_profile_policy_v1", "channel-agnostic native build profile resolver", GROUP_ARTIFACT_TOOLING, SC),
    ("test_channel_agnostic_vehicle_live_gate_policy_v1", "profile-driven read-only vehicle live Gate", GROUP_ARTIFACT_TOOLING, SC),
    ("test_generate_g8_runtime_build_profile_v1", "G8 runtime build-profile binary round trip", GROUP_ARTIFACT_TOOLING, UT),
    ("test_g8_runtime_build_profile_policy_v1", "channel-agnostic G8 runtime build-profile ABI", GROUP_ARTIFACT_TOOLING, SC),
    ("test_g8_runtime_profile_integration_policy_v1", "single-profile G8 payload/controller integration", GROUP_ARTIFACT_TOOLING, SC),
]

# Script-style entries that require extra CLI args beyond a bare invocation.
# Maps module name -> argument list.
SCRIPT_ARGS: dict[str, list[str]] = {
    "validate_fc1_report_v1": ["--selftest"],
    "validate_fc2_report_v1": ["--selftest"],
    "validate_natural_action_lifecycle_report_v1": ["--selftest"],
    "validate_natural_action_scheduler_report_v1": ["--selftest"],
    "validate_fc3_report_v1": ["--selftest"],
    "validate_fc3_phase_map_v1": ["--selftest"],
    "validate_fc3_entry_stability_v1": ["--selftest"],
    "validate_authoritative_neutral_observer_v1": ["--selftest"],
    "validate_authoritative_fixed_delta_v1": ["--selftest"],
    "validate_authoritative_controls_startline_v1": ["--selftest"],
    "validate_authoritative_steering_v1": ["--selftest"],
    "validate_authoritative_steering_search_only_v1": ["--selftest"],
    "validate_final_writer_payload_report_v1": ["--selftest"],
}

# Script-style entries whose offline artifact checks require explicit
# workspace-relative artifact/tool paths.  Every listed file MUST exist:
# a missing artifact is recorded as FAIL, never silently degraded to a
# no-argument run and never marked SKIPPED.
SCRIPT_ARTIFACT_PATHS: dict[str, list[str]] = {
    "test_lifecycle_barrel_source_policy_v1": [
        "android-port/src/hwbp_synchronized_tick_recorder_v1.cpp",
        "android-port/build/lifecycle-natural-action-barrel-source-v1/a9tas_lifecycle_natural_action_barrel_source_v1_review_only",
    ],
    "test_controller_shadow_m1_executor_policy_v1": [
        "android-port/src/controller_shadow_m1_executor_v1.cpp",
    ],
    "test_camera_replay_protocol_policy_v1": [
        "android-port/src/camera_replay_protocol_v1.h",
        "android-port/tools/upstream_src/DetourFunctions.cpp",
        "android-port/tools/upstream_src/ReplayStateManager.cpp",
    ],
    "verify_camera_static_candidate_v1": [
        "android-port/baselines/camera_static_candidate_v1.json",
        "apk-analysis/lib/arm64-v8a/libAsphalt9.so",
    ],
    "test_camera_active_state_resolver_policy_v1": [
        "android-port/src/camera_active_state_resolver_v1.h",
        "android-port/src/camera_active_state_resolver_selftest_v1.cpp",
    ],
    "test_camera_active_state_check_policy_v1": [
        "android-port/build/camera-active-state-check-v1/a9tas_camera_active_state_check_v1_review_only",
    ],
    "test_camera_raceview_record_payload_policy_v1": [
        "android-port/build/camera-raceview-record-v1/liba9tas_camera_raceview_record_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_controller_shadow_phase_paced_executor_policy_v1": [
        "android-port/src/controller_shadow_m1_executor_v1.cpp",
    ],
    "test_controller_shadow_m1_preflight_policy_v1": [
        "android-port/src/controller_shadow_m1_preflight_v1.cpp",
    ],
    "test_m1_three_payload_preload_policy_v1": [
        "android-port/build/m1-three-payload-preload-v1/liba9tas_payload_bundle_natural_controller_v1.so",
        "android-port/build/m1-three-payload-preload-v1/liba9tas_bootstrap_final_writer_m1_v1.so",
    ],
    "test_run_controller_shadow_m1_gate_policy_v1": [
        "android-port/run-controller-shadow-m1-gate-v1.ps1",
    ],
    "test_run_controller_shadow_phase_paced_gate_policy_v1": [
        "android-port/run-controller-shadow-phase-paced-gate-v1.ps1",
    ],
    "test_final_writer_natural_action_runtime_policy_v1": [
        "android-port/build/final-writer-natural-action-runtime-v1/final_writer_natural_action_runtime_selftest_v1_review_only.o",
    ],
    "test_lifecycle_final_writer_natural_action_policy_v1": [
        "android-port/build/lifecycle-final-writer-natural-action-v1/lifecycle_final_writer_natural_action_v1_review_only.o",
    ],
    "audit_natural_action_external_replay_controller_v1": [
        "android-port/build/natural-action-external-replay-controller-v1/natural_action_external_replay_controller_v1_review_only.o",
        "android-port/build/natural-action-external-replay-controller-v1/natural_action_external_replay_controller_v1_review_only",
        "android-port/build/natural-action-external-replay-controller-v1/liba9tas_bootstrap_nar6_v1.so",
        "android-port/build/natural-action-replay-payload-v1/liba9tas_natural_action_replay_v1_review_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "android-port/src/natural_action_lifecycle_controller_v1.cpp",
        "android-port/build-natural-action-external-replay-controller-v1.ps1",
    ],
    "test_final_writer_natural_action_preload_policy_v1": [
        "android-port/build/final-writer-natural-action-live-v1/liba9tas_bootstrap_final_writer_natural_action_v1.so",
    ],
    "test_authoritative_tick_cpp_policy_v1": [
        "android-port/build/authoritative-tick-state-machine-v1/authoritative_tick_state_machine_v1_build_only",
        "android-port/build/authoritative-tick-state-machine-v1/authoritative_tick_state_machine_v1_selftest.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_barrel_stabilization_replay_core_policy_v1": [
        "android-port/build/barrel-stabilization-replay-core-v1/barrel_stabilization_replay_core_v1_build_only",
        "android-port/build/barrel-stabilization-replay-core-v1/barrel_stabilization_replay_core_v1_selftest.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_barrel_yaw_tail_payload_policy_v1": [
        "android-port/build/barrel-yaw-tail-payload-v1/liba9tas_barrel_yaw_tail_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_barrel_yaw_tail_host_transaction_policy_v1": [
        "android-port/build/barrel-yaw-tail-host-transaction-v1/barrel_yaw_tail_host_transaction_v1_selftest",
        "android-port/build/barrel-yaw-tail-host-transaction-v1/barrel_yaw_tail_payload_elf_resolver_v1_selftest",
        "android-port/build/barrel-yaw-tail-payload-v1/liba9tas_barrel_yaw_tail_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
    ],
    "test_barrel_yaw_tail_transport_policy_v1": [
        "android-port/build/barrel-yaw-tail-transport-v1/barrel_yaw_tail_transport_v1_selftest",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
    ],
    "test_barrel_rbx_two_store_transaction_policy_v1": [
        "android-port/build/barrel-rbx-two-store-transaction-v1/barrel_rbx_two_store_transaction_v1_selftest",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
    ],
    "test_barrel_post_c9c_slot_schedule_policy_v1": [
        "android-port/build/barrel-post-c9c-slot-schedule-v1/barrel_post_c9c_slot_schedule_v1_selftest",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
    ],
    "test_barrel_post_c9c_watch_layout_policy_v1": [
        "android-port/build/barrel-post-c9c-watch-layout-v1/barrel_post_c9c_watch_layout_v1_selftest",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
    ],
    "test_barrel_successor_preload_policy_v1": [
        "android-port/build/barrel-successor-preload-v1/liba9tas_payload_bundle_barrel_successor_v1.so",
        "android-port/build/barrel-successor-preload-v1/liba9tas_bootstrap_final_writer_barrel_successor_v1.so",
        "android-port/build/barrel-yaw-tail-payload-v1/liba9tas_barrel_yaw_tail_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
    ],
    "test_authoritative_replay_session_cpp_policy_v1": [
        "android-port/build/authoritative-replay-session-v1/authoritative_replay_session_v1_build_only",
        "android-port/build/authoritative-replay-session-v1/authoritative_replay_session_v1_selftest.o",
        "android-port/build/authoritative-replay-session-v1/authoritative_tick_state_machine_v1_companion.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_unified_adapter_cpp_policy_v1": [
        "android-port/build/authoritative-unified-adapter-v1/authoritative_unified_adapter_v1_build_only",
        "android-port/build/authoritative-unified-adapter-v1/authoritative_unified_adapter_v1_selftest.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_neutral_observer_cpp_policy_v1": [
        "android-port/build/authoritative-neutral-observer-v1/a9tas_hwbp_authoritative_neutral_observer_v1",
        "android-port/build/authoritative-neutral-observer-v1/hwbp_authoritative_neutral_observer_v1.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_fixed_delta_cpp_policy_v1": [
        "android-port/build/authoritative-fixed-delta-v1/a9tas_hwbp_authoritative_fixed_delta_v1",
        "android-port/build/authoritative-fixed-delta-v1/hwbp_authoritative_fixed_delta_v1.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_steering_transport_cpp_policy_v1": [
        "android-port/build/authoritative-steering-transport-v1/authoritative_steering_transport_v1_build_only",
        "android-port/build/authoritative-steering-transport-v1/authoritative_steering_transport_v1_selftest.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_adapter_bridge_cpp_policy_v1": [
        "android-port/build/authoritative-adapter-bridge-v1/authoritative_adapter_bridge_v1_build_only",
        "android-port/build/authoritative-adapter-bridge-v1/authoritative_adapter_bridge_v1_selftest",
        "android-port/build/authoritative-adapter-bridge-v1/authoritative_adapter_bridge_v1.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_natural_handoff_cpp_policy_v1": [
        "android-port/build/authoritative-natural-handoff-v1/authoritative_natural_handoff_v1_build_only",
        "android-port/build/authoritative-natural-handoff-v1/authoritative_natural_handoff_v1_selftest",
        "android-port/build/authoritative-natural-handoff-v1/authoritative_natural_handoff_v1.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_steering_cpp_policy_v1": [
        "android-port/build/authoritative-steering-v1/a9tas_hwbp_authoritative_steering_v1",
        "android-port/build/authoritative-steering-v1/hwbp_authoritative_steering_v1.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_authoritative_steering_search_only_policy_v1": [
        "android-port/build/authoritative-steering-search-only-v1/a9tas_authoritative_steering_search_only_v1",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_frame_callback_bootstrap_policy_v1": [
        "android-port/build/frame-callback-bootstrap-v1/liba9tas_frame_callback_bootstrap_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_final_writer_replay_policy_v1": [
        "android-port/build/final-writer-replay-v1/liba9tas_final_writer_replay_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_final_writer_replay_elf_resolver_v1": [
        "android-port/build/final-writer-replay-v1/liba9tas_final_writer_replay_v1_build_only.so",
    ],
    "test_final_writer_unified_integration_policy_v1": [
        "android-port/build/final-writer-unified-v1/final_writer_unified_v1_build_only",
        "android-port/build/final-writer-unified-v1/final_writer_unified_v1_review_only.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_fc1_payload_elf_resolver_v1": [
        "android-port/build/frame-callback-bootstrap-v1/liba9tas_frame_callback_bootstrap_v1_build_only.so",
    ],
    "test_fc1_frame_callback_transaction_policy_v1": [
        "android-port/build/fc1-frame-callback-transaction-v1/fc1_frame_callback_transaction_v1_build_only",
        "android-port/build/fc1-frame-callback-transaction-v1/fc1_frame_callback_transaction_v1_review_only.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
        "android-port/build/frame-callback-bootstrap-v1/liba9tas_frame_callback_bootstrap_v1_build_only.so",
    ],
    "test_frame_callback_deferred_registration_policy_v1": [
        "android-port/build/frame-callback-deferred-registration-v1/liba9tas_frame_callback_deferred_registration_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_fc2_payload_elf_resolver_v1": [
        "android-port/build/frame-callback-deferred-registration-v1/liba9tas_frame_callback_deferred_registration_v1_build_only.so",
    ],
    "test_natural_action_lifecycle_elf_resolver_v1": [
        "android-port/build/natural-action-callback-lifecycle-v1/liba9tas_natural_action_callback_lifecycle_v1_build_only.so",
    ],
    "audit_natural_action_lifecycle_candidate_v1": [
        "android-port/build/natural-action-lifecycle-live-candidate-v1/natural_action_lifecycle_candidate_v1",
        "android-port/build/natural-action-callback-lifecycle-v1/liba9tas_natural_action_callback_lifecycle_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "android-port/src/natural_action_lifecycle_controller_v1.cpp",
        "android-port/build-natural-action-lifecycle-live-candidate-v1.ps1",
    ],
    "audit_natural_action_scheduler_payload_v1": [
        "android-port/build/natural-action-scheduler-payload-v1/liba9tas_natural_action_scheduler_v1_review_only.so",
        "android-port/build/natural-action-callback-lifecycle-v1/liba9tas_natural_action_callback_lifecycle_v1_build_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
        "android-port/src/payload_natural_action_callback_lifecycle_v1.cpp",
        "android-port/build-natural-action-scheduler-payload-v1.ps1",
    ],
    "test_natural_action_scheduler_elf_resolver_v1": [
        "android-port/build/natural-action-scheduler-payload-v1/liba9tas_natural_action_scheduler_v1_review_only.so",
    ],
    "audit_natural_action_scheduler_controller_v1": [
        "android-port/build/natural-action-scheduler-controller-v1/natural_action_scheduler_controller_v1_review_only.o",
        "android-port/build/natural-action-scheduler-controller-v1/natural_action_scheduler_controller_v1_review_only.disasm.txt",
        "android-port/build/natural-action-scheduler-payload-v1/liba9tas_natural_action_scheduler_v1_review_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "android-port/src/natural_action_lifecycle_controller_v1.cpp",
        "android-port/src/natural_action_scheduler_report_v1.h",
        "android-port/tools/validate_natural_action_scheduler_report_v1.py",
        "android-port/build-natural-action-scheduler-controller-v1.ps1",
        "android-port/build/natural-action-scheduler-controller-v1",
    ],
    "audit_natural_action_scheduler_candidate_v1": [
        "android-port/build/natural-action-scheduler-live-candidate-v1/natural_action_scheduler_candidate_v1",
        "android-port/build/natural-action-scheduler-live-candidate-v1/liba9tas_bootstrap_nas_v1.so",
        "android-port/build/natural-action-scheduler-payload-v1/liba9tas_natural_action_scheduler_v1_review_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "android-port/src/natural_action_lifecycle_controller_v1.cpp",
        "android-port/src/bootstrap_natural_action_scheduler_v1_build.cpp",
        "android-port/build-natural-action-scheduler-live-candidate-v1.ps1",
    ],
    "test_fc2_frame_callback_transaction_policy_v1": [
        "android-port/build/fc2-frame-callback-transaction-v1/fc2_frame_callback_transaction_v1_build_only",
        "android-port/build/fc2-frame-callback-transaction-v1/fc2_frame_callback_transaction_v1_review_only.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "audit_fc2_live_candidate_v1": [
        "android-port/build/frame-callback-deferred-registration-v1/liba9tas_frame_callback_deferred_registration_v1_build_only.so",
        "android-port/build/fc2-runner-v1/a9tas_fc2_frame_callback_controller_v1",
        "android-port/build/fc2-runner-v1/liba9tas_bootstrap_frame_callback_fc2_v1.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_fc3_replay_observer_protocol_policy_v1": [
        "android-port/build/fc3-replay-observer-protocol-v1/fc3_replay_observer_protocol_v1_build_only",
        "android-port/build/fc3-replay-observer-protocol-v1/fc3_replay_observer_protocol_v1_selftest.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_fc3_identity_resolver_policy_v1": [
        "android-port/build/fc3-replay-observer-identity-v1/fc3_replay_observer_identity_v1_build_only",
        "android-port/build/fc3-replay-observer-identity-v1/liba9tas_fc3_identity_resolver_review_only.so",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_fc3_observer_event_core_policy_v1": [
        "android-port/build/fc3-observer-event-core-v1/fc3_observer_event_core_v1_build_only",
        "android-port/build/fc3-observer-event-core-v1/fc3_observer_event_core_v1_review_only.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "test_fc3_successor_controller_policy_v1": [
        "android-port/build/fc3-successor-controller-v1/fc3_successor_controller_v1_build_only",
        "android-port/build/fc3-successor-controller-v1/fc3_successor_controller_v1_review_only.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
    "audit_fc3_successor_candidate_v1": [
        "android-port/build/fc3-successor-controller-v1/fc3_successor_controller_v1_build_only",
        "android-port/build/fc3-successor-controller-v1/fc3_successor_controller_v1_review_only.o",
        "android-port/build/fc3-replay-observer-identity-v1/liba9tas_fc3_identity_resolver_review_only.so",
        "android-port/build/fc3-observer-event-core-v1/fc3_observer_event_core_v1_review_only.o",
        "android-port/build/fc3-replay-observer-protocol-v1/fc3_replay_observer_protocol_v1_selftest.o",
        "android-port/build/fc2-frame-callback-transaction-v1/fc2_frame_callback_transaction_v1_review_only.o",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe",
        "toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe",
    ],
}


def artifact_args(module_name: str) -> tuple[list[str], list[str]]:
    """Resolve required artifact paths for a script entry.

    Returns (absolute_args, missing_paths).  ``missing_paths`` lists the
    workspace-relative paths that do not exist on disk.
    """
    relative = SCRIPT_ARTIFACT_PATHS.get(module_name, [])
    absolute: list[str] = []
    missing: list[str] = []
    for rel in relative:
        path = WORKSPACE_ROOT / rel
        if path.exists():
            absolute.append(str(path))
        else:
            missing.append(rel)
    return absolute, missing


def script_command(module_name: str) -> list[str]:
    """Full ``python <module>.py <args> <artifacts>`` command."""
    script_path = TOOLS_DIR / f"{module_name}.py"
    absolute, _ = artifact_args(module_name)
    return [PY, str(script_path), *SCRIPT_ARGS.get(module_name, []), *absolute]


@dataclasses.dataclass(frozen=True)
class TestResult:
    module: str
    label: str
    group: str
    kind: str
    command: str
    exit_code: int
    duration_ms: int
    status: str
    output_tail: str


def _status_for(exit_code: int) -> str:
    if exit_code == 0:
        return "PASS"
    if exit_code == 5:
        return "SKIPPED"
    return "FAIL"


def _tail(text: str, max_lines: int = 8) -> str:
    lines = text.splitlines()
    if len(lines) <= max_lines:
        return "\n".join(lines)
    return "\n".join(lines[-max_lines:])


def _run_unittest_module(module_name: str) -> tuple[int, str]:
    loader = unittest.TestLoader()
    try:
        suite = loader.loadTestsFromName(module_name)
    except (ImportError, AttributeError, ModuleNotFoundError) as error:
        return 2, f"import_error: {error}\n"
    count = suite.countTestCases()
    if count == 0:
        return 1, f"no tests found in {module_name}\n"
    stream: list[str] = []

    class _CaptureStream:
        def write(self, text: str) -> None:
            stream.append(text)

        def flush(self) -> None:
            pass

    runner = unittest.TextTestRunner(stream=_CaptureStream(), verbosity=1)
    result = runner.run(suite)
    output_lines: list[str] = list(stream)
    if not result.wasSuccessful():
        output_lines.append("FAILURES:")
        for case, trace in result.errors + result.failures:
            output_lines.append(f"{case}:")
            output_lines.extend(trace.splitlines()[-12:])
    return (0 if result.wasSuccessful() else 1), "\n".join(output_lines)


def _run_script_module(module_name: str) -> tuple[int, str]:
    cmd = script_command(module_name)
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120,
            cwd=str(TOOLS_DIR),
        )
    except subprocess.TimeoutExpired:
        return 1, f"timeout after 120s: {' '.join(cmd)}\n"
    output = proc.stdout + proc.stderr
    return proc.returncode, output


def run_suite(allowlist: Iterable[tuple[str, str, str, str]]) -> list[TestResult]:
    results: list[TestResult] = []
    for module_name, label, group, kind in allowlist:
        path = TOOLS_DIR / f"{module_name}.py"
        if kind == UT:
            command = f"python -m unittest {module_name}"
        else:
            relative_artifacts = SCRIPT_ARTIFACT_PATHS.get(module_name, [])
            command = f"python {module_name}.py" + (
                " " + " ".join(SCRIPT_ARGS.get(module_name, []))
                if SCRIPT_ARGS.get(module_name)
                else ""
            ) + (
                " " + " ".join(relative_artifacts)
                if relative_artifacts
                else ""
            )
        if not path.exists():
            results.append(
                TestResult(
                    module=module_name,
                    label=label,
                    group=group,
                    kind=kind,
                    command=command,
                    exit_code=2,
                    duration_ms=0,
                    status="FAIL",
                    output_tail=f"missing test file: {path}",
                )
            )
            continue
        if kind != UT:
            _, missing_artifacts = artifact_args(module_name)
            if missing_artifacts:
                results.append(
                    TestResult(
                        module=module_name,
                        label=label,
                        group=group,
                        kind=kind,
                        command=command,
                        exit_code=2,
                        duration_ms=0,
                        status="FAIL",
                        output_tail=(
                            "required artifact missing: "
                            + "; ".join(missing_artifacts)
                        ),
                    )
                )
                continue
        start = time.perf_counter()
        try:
            if kind == UT:
                exit_code, output = _run_unittest_module(module_name)
            else:
                exit_code, output = _run_script_module(module_name)
        except BaseException as error:  # pragma: no cover - defensive
            exit_code = 3
            output = f"unexpected error: {error!r}"
        duration_ms = int((time.perf_counter() - start) * 1000)
        results.append(
            TestResult(
                module=module_name,
                label=label,
                group=group,
                kind=kind,
                command=command,
                exit_code=exit_code,
                duration_ms=duration_ms,
                status=_status_for(exit_code),
                output_tail=_tail(output),
            )
        )
    return results


def _summary(results: list[TestResult]) -> tuple[int, int, int]:
    passed = sum(1 for r in results if r.status == "PASS")
    failed = sum(1 for r in results if r.status == "FAIL")
    skipped = sum(1 for r in results if r.status == "SKIPPED")
    return passed, failed, skipped


def render_markdown(results: list[TestResult]) -> str:
    passed, failed, skipped = _summary(results)
    lines: list[str] = []
    lines.append("# Offline regression suite v1")
    lines.append("")
    lines.append("Workspace root `" + str(WORKSPACE_ROOT) + "`")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(
        f"OFFLINE_REGRESSION_SUMMARY passed={passed} failed={failed} skipped={skipped} device_access=0"
    )
    lines.append("")
    lines.append(
        f"Total allowlisted tests: {len(results)}; PASS={passed}; FAIL={failed}; "
        f"SKIPPED={skipped}."
    )
    lines.append("")
    lines.append("## Results")
    lines.append("")
    lines.append("| Module | Kind | Group | Label | Status | Exit | Duration (ms) | Command |")
    lines.append("|---|---|---|---|---|---:|---:|---|")
    for r in results:
        lines.append(
            f"| `{r.module}` | {r.kind} | {r.group} | {r.label} | {r.status} | {r.exit_code} | "
            f"{r.duration_ms} | `{r.command}` |"
        )
    lines.append("")
    failed_results = [r for r in results if r.status == "FAIL"]
    if failed_results:
        lines.append("## Failure details")
        lines.append("")
        for r in failed_results:
            lines.append(f"### {r.module}")
            lines.append("")
            lines.append(f"- kind: {r.kind}")
            lines.append(f"- command: `{r.command}`")
            lines.append(f"- exit code: {r.exit_code}")
            lines.append(f"- duration (ms): {r.duration_ms}")
            lines.append("- output tail:")
            lines.append("")
            lines.append("```text")
            lines.append(r.output_tail)
            lines.append("```")
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--markdown-out", type=Path, default=None)
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--stdout-only", action="store_true")
    parser.add_argument("--group", default=None, help="optional group filter")
    args = parser.parse_args(argv)

    allowlist = [
        entry for entry in ALLOWLIST
        if args.group is None or entry[2] == args.group
    ]

    # Fail closed: an empty allowlist after group filtering must never look
    # like a successful run.
    if not allowlist:
        print(
            f"OFFLINE_REGRESSION_ERROR group={args.group!r} matched 0 allowlisted "
            "tests; refusing to report success",
            file=sys.stderr,
        )
        return 2

    results = run_suite(allowlist)
    passed, failed, skipped = _summary(results)

    markdown_path = args.markdown_out or (
        WORKSPACE_ROOT / "android-port" / "evidence" / "OFFLINE_REGRESSION_SUITE_V1.md"
    )
    json_path = args.json_out or (
        WORKSPACE_ROOT / "android-port" / "evidence" / "OFFLINE_REGRESSION_SUITE_V1.json"
    )

    markdown_text = render_markdown(results)
    json_text = json.dumps(
        {
            "workspace_root": str(WORKSPACE_ROOT),
            "device_access": 0,
            "results": [dataclasses.asdict(r) for r in results],
            "summary": {
                "passed": passed,
                "failed": failed,
                "skipped": skipped,
                "total": len(results),
            },
        },
        ensure_ascii=False,
        indent=2,
    ) + "\n"

    if not args.stdout_only:
        markdown_path.write_text(markdown_text, encoding="utf-8")
        json_path.write_text(json_text, encoding="utf-8")

    print(
        f"OFFLINE_REGRESSION_SUMMARY passed={passed} failed={failed} "
        f"skipped={skipped} device_access=0"
    )
    print(f"markdown_out={markdown_path}")
    print(f"json_out={json_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
