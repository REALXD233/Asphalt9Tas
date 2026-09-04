#!/usr/bin/env python3
"""Pure offline tests for the exact-hash Barrel successor source generator."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

import generate_barrel_successor_source_v1 as generator


TOOLS_DIR = Path(__file__).resolve().parent
ANDROID_PORT = TOOLS_DIR.parent

REAL_ANCHORS = {
    "include_barrel_integration":
        '#include "final_writer_unified_integration_v1.h"',
    "runtime_capability_mask":
        "a9tas::unified_tick_v1::kSkipBarrelAngular |",
    "barrel_setup":
        "a9tas::final_writer_unified_v1::Setup(",
    "barrel_runtime_state":
        "pid_t post_phase_tid = 0;",
    "c9c_barrel_layout":
        "world_accumulator, PipelineDr7())) {",
    "post_phase_dr0":
        'semantic_fault("unexpected_completion", tid, dr6);',
    "f64_barrel_terminal":
        "!Advance(&machine, Event::kF64, false, false,",
    "barrel_deferred_release":
        "machine.stage == Stage::kWaitDeferredClear",
    "world_barrel_commit":
        "CommitAtWorldBoundary(",
    "barrel_cleanup":
        "const bool final_writer_cleanup_frozen =",
    "barrel_success_condition":
        "success = success && final_writer_final_ok &&",
}


def replacement_item(anchor_id: str, old: str | None = None) -> dict[str, str]:
    original = REAL_ANCHORS[anchor_id] if old is None else old
    begin = f"// A9TAS_GENERATED_BEGIN anchor={anchor_id}"
    end = f"// A9TAS_GENERATED_END anchor={anchor_id}"
    return {
        "id": anchor_id,
        "old_utf8": original,
        "new_utf8": f"{begin}\n{original}\n{end}",
    }


def plan_document(*, reverse: bool = False) -> dict[str, object]:
    replacements = [replacement_item(anchor) for anchor in generator.REQUIRED_ANCHOR_IDS]
    if reverse:
        replacements.reverse()
    return {
        "schema": generator.PLAN_SCHEMA,
        "complete_successor": True,
        "required_anchor_ids": list(generator.REQUIRED_ANCHOR_IDS),
        "replacements": replacements,
    }


def write_plan(path: Path, document: dict[str, object], *, compact: bool = False) -> None:
    if compact:
        payload = json.dumps(document, separators=(",", ":"), ensure_ascii=False)
    else:
        payload = json.dumps(document, indent=2, ensure_ascii=False)
    path.write_text(payload + "\n", encoding="utf-8", newline="\n")


def copy_pinned_fixture(destination: Path) -> Path:
    root = destination / "android-port"
    for relative in generator.PINNED_SHA256:
        source = ANDROID_PORT / relative
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    return root


class BarrelSuccessorSourceGeneratorTests(unittest.TestCase):
    def test_real_inputs_match_all_five_pins(self) -> None:
        inputs = generator.verify_pinned_inputs(ANDROID_PORT)
        self.assertEqual(set(inputs), set(generator.PINNED_SHA256))
        self.assertEqual(len(inputs), 5)

    def test_valid_generation_copies_wrappers_and_publishes_manifest(self) -> None:
        before = generator.verify_pinned_inputs(ANDROID_PORT)
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            plan_path = temp / "plan.json"
            output = temp / "build" / "successor"
            write_plan(plan_path, plan_document())
            manifest = generator.generate(ANDROID_PORT, plan_path, output)

            self.assertTrue(output.is_dir())
            self.assertEqual(manifest["schema"], generator.MANIFEST_SCHEMA)
            self.assertTrue(manifest["complete_successor"])
            self.assertTrue(manifest["baseline_unchanged"])
            on_disk = json.loads((output / generator.MANIFEST_NAME).read_text("utf-8"))
            self.assertEqual(on_disk, manifest)
            self.assertEqual(
                (output / generator.ENTRY_NAME).read_bytes(), generator.ENTRY_BYTES
            )
            for relative in generator.WRAPPER_PATHS:
                self.assertEqual(
                    (output / Path(relative).name).read_bytes(), before[relative]
                )
            derived = (output / Path(generator.EXECUTOR_PATH).name).read_bytes()
            self.assertNotEqual(derived, before[generator.EXECUTOR_PATH])
            for anchor in generator.REQUIRED_ANCHOR_IDS:
                self.assertEqual(
                    derived.count(
                        f"// A9TAS_GENERATED_BEGIN anchor={anchor}".encode("ascii")
                    ),
                    1,
                )
                self.assertEqual(
                    derived.count(
                        f"// A9TAS_GENERATED_END anchor={anchor}".encode("ascii")
                    ),
                    1,
                )
        self.assertEqual(before, generator.verify_pinned_inputs(ANDROID_PORT))

    def test_semantically_identical_plan_order_and_format_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            first_plan = temp / "first.json"
            second_plan = temp / "second.json"
            write_plan(first_plan, plan_document(), compact=False)
            write_plan(second_plan, plan_document(reverse=True), compact=True)
            first_output = temp / "build" / "one"
            second_output = temp / "build" / "two"
            first_manifest = generator.generate(ANDROID_PORT, first_plan, first_output)
            second_manifest = generator.generate(ANDROID_PORT, second_plan, second_output)
            self.assertEqual(first_manifest, second_manifest)
            first_files = {
                path.name: path.read_bytes() for path in first_output.iterdir()
            }
            second_files = {
                path.name: path.read_bytes() for path in second_output.iterdir()
            }
            self.assertEqual(first_files, second_files)

    def test_each_pinned_input_hash_drift_fails_before_output(self) -> None:
        for relative in generator.PINNED_SHA256:
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as temporary:
                temp = Path(temporary)
                fixture = copy_pinned_fixture(temp)
                candidate = fixture / relative
                candidate.write_bytes(candidate.read_bytes() + b"\n")
                plan_path = temp / "plan.json"
                output = temp / "build" / "successor"
                write_plan(plan_path, plan_document())
                with self.assertRaisesRegex(generator.GenerationError, "hash drift"):
                    generator.generate(fixture, plan_path, output)
                self.assertFalse(output.exists())

    def test_incomplete_and_unmarked_plans_are_rejected(self) -> None:
        cases: list[tuple[str, dict[str, object], str]] = []
        incomplete = plan_document()
        incomplete["complete_successor"] = False
        cases.append(("incomplete", incomplete, "complete_successor"))

        missing = plan_document()
        missing["replacements"] = list(missing["replacements"])[1:]
        cases.append(("missing", missing, "incomplete"))

        unmarked = plan_document()
        first = dict(list(unmarked["replacements"])[0])
        first["new_utf8"] = first["old_utf8"] + " changed"
        unmarked["replacements"] = [first] + list(unmarked["replacements"])[1:]
        cases.append(("unmarked", unmarked, "BEGIN/END"))

        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            for name, document, message in cases:
                with self.subTest(name=name):
                    path = temp / f"{name}.json"
                    write_plan(path, document)
                    with self.assertRaisesRegex(generator.GenerationError, message):
                        generator.load_replacement_plan(path)

    def test_duplicate_json_keys_and_unknown_root_fields_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            duplicate = temp / "duplicate.json"
            duplicate.write_text(
                '{"schema":"one","schema":"two"}\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(generator.GenerationError, "duplicate JSON key"):
                generator.load_replacement_plan(duplicate)

            unknown_document = plan_document()
            unknown_document["unreviewed_metadata"] = "rejected"
            unknown = temp / "unknown.json"
            write_plan(unknown, unknown_document)
            with self.assertRaisesRegex(generator.GenerationError, "root keys"):
                generator.load_replacement_plan(unknown)

    def test_missing_duplicate_and_overlapping_anchors_fail_closed(self) -> None:
        anchor_a, anchor_b = generator.REQUIRED_ANCHOR_IDS[:2]

        def synthetic_plan(first: bytes, second: bytes) -> generator.ReplacementPlan:
            replacements = []
            for index, anchor in enumerate(generator.REQUIRED_ANCHOR_IDS):
                old = first if index == 0 else second if index == 1 else f"u{index}".encode()
                replacements.append(generator.Replacement(anchor, old, b"new-" + old))
            return generator.ReplacementPlan(tuple(replacements), b"", "")

        suffix = b" ".join(
            f"u{index}".encode()
            for index in range(2, len(generator.REQUIRED_ANCHOR_IDS))
        )
        missing_plan = synthetic_plan(b"absent", b"present")
        with self.assertRaisesRegex(generator.GenerationError, "count is 0"):
            generator.apply_replacements(b"present " + suffix, missing_plan)

        duplicate_plan = synthetic_plan(b"same", b"present")
        with self.assertRaisesRegex(generator.GenerationError, "count is 2"):
            generator.apply_replacements(b"same same present " + suffix, duplicate_plan)

        overlap_plan = synthetic_plan(b"abcde", b"cde")
        with self.assertRaisesRegex(generator.GenerationError, "overlap"):
            generator.apply_replacements(b"abcdef " + suffix, overlap_plan)

        incomplete_plan = generator.ReplacementPlan(
            (generator.Replacement(anchor_a, b"a", b"b"),), b"", ""
        )
        with self.assertRaisesRegex(generator.GenerationError, "not complete"):
            generator.apply_replacements(b"a", incomplete_plan)

    def test_atomic_publish_failure_leaves_no_output_or_staging(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            plan_path = temp / "plan.json"
            output = temp / "build" / "successor"
            write_plan(plan_path, plan_document())
            with mock.patch.object(generator.os, "replace", side_effect=OSError("forced")):
                with self.assertRaisesRegex(generator.GenerationError, "atomic"):
                    generator.generate(ANDROID_PORT, plan_path, output)
            self.assertFalse(output.exists())
            self.assertEqual(
                list(output.parent.glob(f".{output.name}.tmp-*")), []
            )

    def test_existing_output_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            plan_path = temp / "plan.json"
            output = temp / "build" / "successor"
            output.mkdir(parents=True)
            sentinel = output / "owned.txt"
            sentinel.write_text("preserve", encoding="utf-8")
            write_plan(plan_path, plan_document())
            with self.assertRaisesRegex(generator.GenerationError, "already exists"):
                generator.generate(ANDROID_PORT, plan_path, output)
            self.assertEqual(sentinel.read_text("utf-8"), "preserve")

    def test_check_only_performs_no_output_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            plan_path = temp / "plan.json"
            output = temp / "would-have-been-output"
            write_plan(plan_path, plan_document())
            manifest = generator.generate(
                ANDROID_PORT, plan_path, output, check_only=True
            )
            self.assertEqual(manifest["schema"], generator.MANIFEST_SCHEMA)
            self.assertFalse(output.exists())

    def test_output_scope_rejects_source_tree(self) -> None:
        with self.assertRaisesRegex(generator.GenerationError, "below src"):
            generator._validate_output_scope(
                ANDROID_PORT, ANDROID_PORT / "src" / "generated-successor"
            )
        with self.assertRaisesRegex(generator.GenerationError, "inside build"):
            generator._validate_output_scope(
                ANDROID_PORT, ANDROID_PORT / "tools" / "generated-successor"
            )


if __name__ == "__main__":
    unittest.main()
