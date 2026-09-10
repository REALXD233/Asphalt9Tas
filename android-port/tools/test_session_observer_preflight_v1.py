"""Regression checks for read-only discovery before passive hook publication."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PreflightTests(unittest.TestCase):
    def test_discovery_and_decode_precede_mutation(self):
        source = (ROOT / 'A9TasAndroid/app/src/main/java/dev/a9tas/android/SessionOrchestrator.java').read_text(encoding='utf-8')
        method = source.split('static InstallReceipt installPassive(Context context)', 1)[1].split('static String restoreIfNeeded', 1)[0]
        self.assertLess(method.index('PhysicsIntervalReceipt.parse('), method.index('installAttempted = true'))
        self.assertLess(method.index('installAttempted = true'), method.index('identity, "install"'))
        self.assertIn('if (installAttempted) rollbackOrTerminate', method)
        self.assertEqual(method.count('String observation ='), 1)

    def test_scan_failure_does_not_skip_chunk(self):
        source = (ROOT / 'src/physics_interval_readonly_observer_v1.cpp').read_text(encoding='utf-8')
        branch = source.split('if (got <= 0)', 1)[1].split('continue;', 1)[0]
        self.assertNotIn('cursor += want', branch)
        self.assertIn('page_size - cursor % page_size', branch)
        self.assertIn('vtable_hits=%zu read_failures=%zu', source)


if __name__ == '__main__':
    unittest.main()
