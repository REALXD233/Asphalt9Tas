from __future__ import annotations

import hashlib
import unittest

import compare_final_writer_trajectory_runs_v1 as compare_v1
import make_final_writer_target_blob_v1 as target_v1
import validate_final_writer_payload_report_v1 as report_v1


class FinalWriterTrajectoryComparisonTest(unittest.TestCase):
    @staticmethod
    def fixtures(before_second: bytes, pid: int) -> tuple[bytes, bytes, bytes]:
        source_hash = hashlib.sha256(b"same-source").digest()
        targets = (
            (bytes(64), bytes(12)),
            (bytes([1]) * 64, bytes([2]) * 12),
        )
        target = bytearray(target_v1._HEADER.pack(
            target_v1.MAGIC, target_v1.VERSION,
            target_v1.HEADER_SIZE, target_v1.RECORD_SIZE, 2,
            target_v1.REQUIRED_INTERVAL_US, target_v1.REQUIRED_FLAGS,
            len(b"same-source"), 0, source_hash,
            target_v1.SUPPORTED_BUILD_ID, bytes(36)))
        for transform, linear in targets:
            target += target_v1._TARGET.pack(transform, linear, 0)

        payload = b"same-payload"
        shadow_vptr = 0x2200
        original_vptr = 0x1100
        equal_count = int(before_second == targets[1][0])
        corrected_count = 1 - equal_count
        evidence_prefix = report_v1._EVIDENCE_PREFIX.pack(
            report_v1.EVIDENCE_MAGIC, 1, report_v1.EVIDENCE_SIZE,
            2, 2, 2, 1 + equal_count, corrected_count,
            corrected_count * 2, 0, 0, 2, 1,
            0x3300, 0x4400, shadow_vptr, original_vptr, 7, 0, 0, 0)
        evidence = evidence_prefix + bytes(
            report_v1.EVIDENCE_SIZE - len(evidence_prefix))
        header = report_v1._HEADER.pack(
            report_v1.MAGIC, 1, report_v1.HEADER_SIZE,
            report_v1.AUDIT_SIZE, report_v1.REQUIRED_FLAGS, 2, 0,
            pid, 0x100000, 0x3300, 0x5500, shadow_vptr, original_vptr,
            0x6600, 0x7700, 0x8800, 0x9900,
            source_hash, hashlib.sha256(payload).digest(), evidence)
        audit0 = report_v1._AUDIT.pack(
            0, report_v1.AUDIT_ORIGINAL_RETURNED | report_v1.AUDIT_EQUAL |
            report_v1.AUDIT_IMMEDIATE_EXACT,
            targets[0][0], targets[0][1], targets[0][0], targets[0][1])
        second_flags = (
            report_v1.AUDIT_EQUAL if equal_count else report_v1.AUDIT_CORRECTED
        )
        audit1 = report_v1._AUDIT.pack(
            1, report_v1.AUDIT_ORIGINAL_RETURNED | second_flags |
            report_v1.AUDIT_IMMEDIATE_EXACT | report_v1.AUDIT_FINAL_FRAME |
            report_v1.AUDIT_VPTR_RESTORED,
            before_second, targets[1][1], targets[1][0], targets[1][1])
        return bytes(target), payload, header + audit0 + audit1

    def test_identical_pre_correction_trajectories_pass(self) -> None:
        target, payload, run0 = self.fixtures(bytes([1]) * 64, 101)
        _, _, run1 = self.fixtures(bytes([1]) * 64, 202)
        result = compare_v1.compare_runs([run0, run1], target, payload)
        self.assertEqual(result["all_pre_correction_trajectories_identical"], 1)
        self.assertEqual(result["comparisons"][0]["first_divergence_frame"], -1)

    def test_first_natural_divergence_is_reported(self) -> None:
        target, payload, run0 = self.fixtures(bytes([1]) * 64, 101)
        changed = bytes([9]) + bytes([1]) * 63
        _, _, run1 = self.fixtures(changed, 202)
        result = compare_v1.compare_runs([run0, run1], target, payload)
        pair = result["comparisons"][0]
        self.assertEqual(result["all_pre_correction_trajectories_identical"], 0)
        self.assertEqual(pair["first_divergence_frame"], 1)
        self.assertEqual(pair["transform_differences"], 1)
        self.assertEqual(pair["linear_differences"], 0)
        self.assertEqual(pair["correction_class_differences"], 1)

    def test_different_source_is_rejected(self) -> None:
        target, payload, run0 = self.fixtures(bytes([1]) * 64, 101)
        _, _, run1 = self.fixtures(bytes([1]) * 64, 202)
        damaged = bytearray(run1)
        damaged[144] ^= 1
        with self.assertRaises(ValueError):
            compare_v1.compare_runs([run0, bytes(damaged)], target, payload)


if __name__ == "__main__":
    unittest.main()
