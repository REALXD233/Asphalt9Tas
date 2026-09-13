"""Regression: teardown must precede physics sampling in the final callback."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root/'src/payload_g4_multi_hook_runtime_v1.cpp').read_text(encoding='utf-8')
body = source.split('G4FinalAfterV1(void* player) {',1)[1].split('bridge::ObserveFinalWriterReturn',1)[0]
assert body.index('MaybeDiscardOpenLifecycleTick(lifecycle)') < body.index('MaybeCompleteLifecycleRecording(lifecycle)')
assert 'UnlockRuntime();\n    return;' in body
discard = source.split('bool MaybeDiscardOpenLifecycleTick(',1)[1].split('// Race end',1)[0]
assert 'RunMode::kRecord' in discard and 'lifecycle == 3u' in discard
assert 'RestoreDiscardedTickSemantics(tick)' in discard
print('FINAL_LIFECYCLE_ORDER passed=1 recording_only=1 existing_discard_reused=1')
