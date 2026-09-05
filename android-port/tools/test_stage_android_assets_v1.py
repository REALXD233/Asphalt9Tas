import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from stage_android_assets_v1 import stage


class AssetStageTests(unittest.TestCase):
    def test_manifest_is_the_complete_runtime_allowlist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            (source / "runtime").mkdir(parents=True)
            (source / "profiles").mkdir()
            (source / "profiles/registry.json").write_text('{"schema":1,"profiles":[1]}')
            (source / "profiles/known.bin").write_bytes(b"profile")
            artifact = {"asset": "runtime/active", "sha256": hashlib.sha256(b"current").hexdigest()}
            manifest = {"identity_helpers": [artifact], "artifact_sets": [{"artifacts": [artifact]}]}
            (source / "runtime/manifest.json").write_text(json.dumps(manifest))
            (source / "runtime/active").write_bytes(b"current")
            (source / "runtime/old").write_bytes(b"old")
            for empty in (False, True):
                output = stage(source, root / "build", empty)
                self.assertEqual({p.name for p in (output / "assets/runtime").iterdir()},
                                 {"active", "manifest.json"})
                self.assertEqual((output / "assets/profiles/known.bin").exists(), not empty)
                if empty:
                    self.assertEqual(json.loads((output / "assets/profiles/registry.json").read_text())["profiles"], [])
            self.assertTrue((source / "runtime/old").exists())
            (source / "runtime/active").write_bytes(b"broken")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                stage(source, root / "build")


if __name__ == "__main__":
    unittest.main()
