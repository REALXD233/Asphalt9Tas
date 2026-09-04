#!/usr/bin/env python3
"""Offline tests for the user workflow and error copy draft (B6/N2-R1).

Reads only the Markdown draft; never touches a device or any game state.
Covers the original 13 principle tests plus the N2-R1 corrections:
recording-mode split, preparation semantics, replay-ready semantics and
cleanup-state semantics (per-section checks).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

DOC = Path(__file__).resolve().parent.parent / "docs" / "USER_WORKFLOW_AND_ERRORS_DRAFT_20260818.md"

REQUIRED_STATES = (
    "OFFLINE_VALIDATING",
    "OFFLINE_INVALID",
    "PREPARE_CONFIRMATION_REQUIRED",
    "PREPARING_NEW_PROCESS",
    "WAITING_FOR_RACE",
    "WAITING_FOR_NATURAL_RUN",
    "RECORDING",
    "RECORDING_ZERO_INPUT",
    "RECORDING_USER_ACTION",
    "RECORDING_FIXED_DELTA",
    "RETRY_REQUIRED",
    "REPLAY_READY",
    "REPLAYING",
    "SUCCESS",
    "FAIL_CLOSED",
    "PROCESS_CHANGED",
    "TRACERPID_NONZERO",
    "HASH_MISMATCH",
    "REPORT_EXISTS",
    "GAME_CRASHED",
    "USER_ABORTED",
    "RECOVERY_TO_LOBBY",
)

ORIGINAL_19 = tuple(state for state in REQUIRED_STATES
                    if state not in ("RECORDING_ZERO_INPUT", "RECORDING_USER_ACTION",
                                     "RECORDING_FIXED_DELTA"))

STATE_HEADING_RE = re.compile(r"^### 状态:([A-Z0-9_]+)\s*$", re.MULTILINE)


def doc_text() -> str:
    return DOC.read_text(encoding="utf-8")


def state_sections(text: str) -> dict[str, str]:
    matches = list(STATE_HEADING_RE.finditer(text))
    sections: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(1)] = text[match.end():end]
    return sections


class StateCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = doc_text()
        cls.sections = state_sections(cls.text)

    def test_all_required_state_ids_defined_exactly_once(self) -> None:
        headings = [m.group(1) for m in STATE_HEADING_RE.finditer(self.text)]
        for state_id in REQUIRED_STATES:
            with self.subTest(state=state_id):
                self.assertEqual(headings.count(state_id), 1, f"{state_id} defined {headings.count(state_id)} times")
        self.assertEqual(len(REQUIRED_STATES), len(set(REQUIRED_STATES)))

    def test_original_19_states_still_present(self) -> None:
        headings = [m.group(1) for m in STATE_HEADING_RE.finditer(self.text)]
        for state_id in ORIGINAL_19:
            self.assertEqual(headings.count(state_id), 1, f"original state {state_id} missing or duplicated")

    def test_state_count_is_at_least_22_and_not_hardcoded(self) -> None:
        headings = [m.group(1) for m in STATE_HEADING_RE.finditer(self.text)]
        self.assertGreaterEqual(len(headings), 22)
        # The draft may add more states later; only a lower bound is asserted.

    def test_every_state_has_chinese_title_and_next_step(self) -> None:
        for state_id in REQUIRED_STATES:
            with self.subTest(state=state_id):
                section = self.sections[state_id]
                self.assertIn("- 中文标题:", section)
                self.assertIn("- 用户下一步:", section)
                title_line = next(line for line in section.splitlines() if line.startswith("- 中文标题:"))
                self.assertTrue(re.search(r"[\u4e00-\u9fff]", title_line), title_line)

    def test_write_phase_marker_on_every_state(self) -> None:
        for state_id in REQUIRED_STATES:
            with self.subTest(state=state_id):
                self.assertIn("- 系统写入:", self.sections[state_id])

    def test_safety_evidence_marker_on_every_state(self) -> None:
        for state_id in REQUIRED_STATES:
            with self.subTest(state=state_id):
                self.assertIn("- 安全/保留证据行为:", self.sections[state_id])


class ContentPrincipleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = doc_text()

    def test_device_access_zero_scope_explained(self) -> None:
        self.assertIn("device_access=0", self.text)
        self.assertIn("仅表示", self.text)
        self.assertIn("离线", self.text)

    def test_required_topics_present(self) -> None:
        for topic in ("Retry", "进程", "TracerPid", "哈希", "报告已存在", "崩溃"):
            self.assertIn(topic, self.text)

    def test_preserve_reports_and_no_auto_delete(self) -> None:
        self.assertIn("保留", self.text)
        self.assertIn("不自动删除", self.text)
        self.assertIn("报告", self.text)
        self.assertIn("日志", self.text)

    def test_no_auto_retry_no_overwrite_no_bypass(self) -> None:
        self.assertIn("不自动重试", self.text)
        self.assertIn("不自动覆盖", self.text)
        self.assertTrue(
            "明确确认" in self.text or "明确点击" in self.text,
            "draft must require explicit confirmation",
        )
        self.assertIn("一键绕过确认", self.text)  # present as a prohibition
        self.assertIn("不会自动", self.text)

    def test_fc1_never_written_as_live_pass(self) -> None:
        for line in self.text.splitlines():
            lowered = line.lower()
            if "fc-1" in lowered and re.search(r"live\s*pass", lowered):
                self.fail(f"FC-1 written as live pass: {line!r}")
        self.assertIn("BUILD_ONLY", self.text)
        self.assertIn("OFFLINE", self.text)

    def test_no_pt_nb_retry_advice(self) -> None:
        for needle in ("重试 PT-NB0", "重试 PT-NB1", "再试 PT-NB", "重新尝试 PT-NB"):
            self.assertNotIn(needle, self.text)
        self.assertIn("永久退役", self.text)
        self.assertIn("PT-NB0", self.text)

    def test_structural_pass_not_equal_to_tas_success(self) -> None:
        self.assertIn("结构校验", self.text)
        self.assertIn("结构校验通过 ≠ replay 成功", self.text)
        for forbidden in ("结构校验通过 = replay 成功", "结构校验通过即回放成功",
                          "结构校验通过,回放成功"):
            self.assertNotIn(forbidden, self.text)

    def test_no_never_crash_promise(self) -> None:
        self.assertNotIn("绝不会崩溃", self.text)

    def test_cancel_stops_next_phase(self) -> None:
        self.assertIn("取消", self.text)
        self.assertIn("不会继续下一阶段", self.text)


class RecordingSemanticsTests(unittest.TestCase):
    """R1: recording is not a uniform fully-read-only process."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = doc_text()
        cls.sections = state_sections(cls.text)

    def test_fixed_delta_and_three_write_classes_disclosed(self) -> None:
        self.assertIn("fixed-delta", self.text)
        self.assertIn("delta writes", self.text)
        self.assertIn("control writes", self.text)
        self.assertIn("physics/final writes", self.text)

    def test_zero_input_and_user_action_modes_both_present(self) -> None:
        zero = self.sections["RECORDING_ZERO_INPUT"]
        action = self.sections["RECORDING_USER_ACTION"]
        self.assertIn("零输入", zero)
        self.assertIn("gameplay_writes=0", zero)
        self.assertIn("trigger", zero)
        self.assertIn("用户动作", action)

    def test_action_prompts_cover_steering_brake_drift_nitro(self) -> None:
        action = self.sections["RECORDING_USER_ACTION"]
        self.assertIn("steering", action)
        self.assertIn("brake/drift", action)
        self.assertIn("nitro", action)
        self.assertIn("开始条件", action)
        self.assertIn("结束条件", action)

    def test_action_mismatch_fails_closed_without_retry(self) -> None:
        action = self.sections["RECORDING_USER_ACTION"]
        self.assertIn("误输入", action)
        self.assertIn("停止", action)
        self.assertIn("保留报告", action)
        self.assertIn("不自动重试", action)

    def test_fixed_delta_mode_discloses_budget_and_budget_breach(self) -> None:
        delta = self.sections["RECORDING_FIXED_DELTA"]
        self.assertIn("最大帧数", delta)
        self.assertIn("最大写入次数", delta)
        self.assertIn("已授权计划", delta)
        self.assertIn("超预算", delta)
        self.assertIn("fail closed", delta)

    def test_generic_recording_does_not_claim_read_only(self) -> None:
        generic = self.sections["RECORDING"]
        self.assertIn("录制模式", generic)
        self.assertNotIn("只读观察", generic)
        self.assertNotIn("不写入游戏内存", generic)

    def test_forbidden_unqualified_claims_absent(self) -> None:
        self.assertNotIn("观察不会写入游戏内存", self.text)
        self.assertNotIn("录制不会写入游戏内存", self.text)

    def test_generic_recording_and_user_action_do_not_demand_no_operation(self) -> None:
        for state_id in ("RECORDING", "RECORDING_USER_ACTION"):
            with self.subTest(state=state_id):
                section = self.sections[state_id]
                self.assertNotIn("不要操作", section)
                self.assertNotIn("不要进行任何操作", section)


class PreparationAndReadySemanticsTests(unittest.TestCase):
    """R2/R3: preparation and replay-ready must not over-promise."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.sections = state_sections(doc_text())

    def test_preparing_discloses_no_formal_replay_writes_yet(self) -> None:
        section = self.sections["PREPARING_NEW_PROCESS"]
        self.assertIn("尚未开始正式回放/动作/车辆物理写入", section)
        self.assertIn("bootstrap", section)
        self.assertIn("收据", section)
        self.assertIn("计数", section)
        self.assertIn("PID/start-time", section)

    def test_preparing_does_not_promise_zero_in_process_writes(self) -> None:
        section = self.sections["PREPARING_NEW_PROCESS"]
        self.assertNotIn("绝对零进程内写入", section)
        self.assertNotIn("零进程内写入", section)
        # It must explicitly refuse the promise instead.
        self.assertIn("不承诺", section)

    def test_replay_ready_does_not_claim_unchanged_game_state(self) -> None:
        section = self.sections["REPLAY_READY"]
        self.assertNotIn("游戏状态未被修改", section)
        self.assertNotIn("整个游戏状态没有变化", section)
        self.assertNotIn("游戏状态从未改变", section)
        self.assertIn("正式回放写入尚未开始", section)


class CleanupSemanticsTests(unittest.TestCase):
    """R4: cleanup/restore/rollback must be separated and report-driven."""

    CLEANUP_STATES = (
        "CLEANUP_NOT_NEEDED",
        "CLEANUP_CONFIRMED",
        "CLEANUP_ATTEMPTED",
        "CLEANUP_UNKNOWN",
    )

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = doc_text()
        cls.sections = state_sections(cls.text)

    def test_four_cleanup_state_words_defined(self) -> None:
        for word in self.CLEANUP_STATES:
            self.assertIn(word, self.text)

    def test_tool_cleanup_not_equal_to_world_rollback(self) -> None:
        self.assertIn("工具清理不等于游戏世界状态回滚", self.text)

    def test_game_crashed_defaults_to_cleanup_unknown(self) -> None:
        section = self.sections["GAME_CRASHED"]
        self.assertIn("CLEANUP_UNKNOWN", section)
        self.assertNotIn("已完成清理/回滚", section)
        # The draft must state the prohibition, not claim rollback happened.
        self.assertIn("不得声称崩溃前动作已回滚", section)

    def test_success_confirmed_only_with_report_proof(self) -> None:
        section = self.sections["SUCCESS"]
        self.assertIn("CLEANUP_CONFIRMED", section)
        self.assertIn("仅当", section)
        self.assertIn("报告证明", section)

    def test_fail_closed_has_no_unconditional_rollback_claim(self) -> None:
        section = self.sections["FAIL_CLOSED"]
        self.assertNotIn("已完成回滚", section)
        self.assertIn("CLEANUP_NOT_NEEDED", section)
        self.assertIn("CLEANUP_UNKNOWN", section)

    def test_user_aborted_has_no_unconditional_rollback_claim(self) -> None:
        section = self.sections["USER_ABORTED"]
        self.assertNotIn("已完成回滚", section)
        self.assertIn("CLEANUP_NOT_NEEDED", section)

    def test_process_changed_cleanup_may_be_unknown(self) -> None:
        section = self.sections["PROCESS_CHANGED"]
        self.assertIn("CLEANUP_UNKNOWN", section)

    def test_prewrite_detection_states_use_not_needed_or_pending(self) -> None:
        for state_id in ("TRACERPID_NONZERO", "HASH_MISMATCH", "REPORT_EXISTS"):
            with self.subTest(state=state_id):
                section = self.sections[state_id]
                self.assertTrue(
                    "CLEANUP_NOT_NEEDED" in section or "等待报告判定" in section,
                    f"{state_id} must state a cleanup status",
                )

    def test_forbidden_unconditional_cleanup_claims_absent(self) -> None:
        for forbidden in (
            "游戏崩溃后已完成清理/回滚",
            "若有部分写入,已完成清理/回滚",
            "失败时回滚并保留",
            "成功/失败收尾 | 已完成清理/回滚",
            "游戏状态已回滚",
        ):
            self.assertNotIn(forbidden, self.text)


if __name__ == "__main__":
    unittest.main()
