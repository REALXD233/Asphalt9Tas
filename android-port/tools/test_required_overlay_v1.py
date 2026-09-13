from pathlib import Path
root = Path(__file__).resolve().parents[1]
java = root/'A9TasAndroid/app/src/main/java/dev/a9tas/android'
service = (java/'TasForegroundService.java').read_text(encoding='utf-8')
overlay = (java/'TasOverlayController.java').read_text(encoding='utf-8')
replay = (java/'SessionOrchestrator.java').read_text(encoding='utf-8')
assert '!overlayController.show() || !overlayController.isAttached()' in service
assert 'overlayGuardHandler.removeCallbacks(overlayGuard)' in service
assert 'runExclusive("recovery", this::resumeHardPausedBranch)' in service
assert 'overlayController.collapse();' in service
assert '"TAS\\n"' in overlay
assert 'snapToEdge' not in overlay
assert 'preferences.getInt("overlay_x", params.x)' in overlay
assert 'preferences.getInt("overlay_y", params.y)' in overlay
assert 'RippleDrawable' in overlay
assert '回放未开始：未找到新局倒计时锚点' in replay
assert 'if (!base.archiveSha256.equals(binding)) target = base.summary.targetTick;' in service
print('REQUIRED_OVERLAY_POLICY passed=1 marker=1 recovery_allowed=1 anchor_preserved=1')
