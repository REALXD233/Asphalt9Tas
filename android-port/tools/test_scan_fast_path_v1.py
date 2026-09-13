"""Keep the batched fast path advisory and the unknown-package fallback intact."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] /
          'A9TasAndroid/app/src/main/java/dev/a9tas/android/GameProcessScanner.java').read_text(encoding='utf-8')
assert 'ps -A -o PID,NAME' in source
assert 'for p in $fast_pids' in source
assert 'found_count' in source
assert 'scan_one \\"$d\\" all' in source
assert 'libAsphalt9' in source
assert 'for d in /proc/[0-9]*; do scan_one \\"$d\\" fast' not in source
assert 'RootShell.runFixedScript(FIXED_SCAN_SCRIPT, 45L)' in source
assert 'Map<String, LinkedHashSet<String>> pathsByMachine = new LinkedHashMap<>()' in source
assert 'entry.getValue().toArray(new String[0])' in source
assert 'hashesByMachine.get(fields[5])' in source
assert 'fields[5], fields[4], fields[9]' not in source
print('SCAN_FAST_PATH_POLICY passed=1 batch_ps=1 unknown_package_fallback=1 unchanged_timeout=1')
