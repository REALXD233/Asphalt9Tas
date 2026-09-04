# Creates only the reviewed /data/local/tmp host trigger. It sends no game input.
param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [switch]$AcknowledgeNeutralControlsAndStart180FrameWindow,
    [switch]$AcknowledgeNeutralControlsAndStartActionCapture
)

$ErrorActionPreference = "Stop"
if (-not ($AcknowledgeNeutralControlsAndStart180FrameWindow -or
          $AcknowledgeNeutralControlsAndStartActionCapture)) {
    throw "Must acknowledge neutral controls and the action capture start"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }
$package = "com.aligames.kuang.kybc.aligames"
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') { throw "Game PID unavailable: '$pidText'" }
$tracer = (& $AdbPath -s $Device shell "cat /proc/$pidText/status | grep '^TracerPid:'").Trim()
if ($tracer -notmatch '^TracerPid:\s*([1-9][0-9]*)$') { throw "Reviewed recorder is not attached: '$tracer'" }
$triggerPath = "/data/local/tmp/a9tas_action_window_start_$pidText"
$before = (& $AdbPath -s $Device shell "su -c 'test -e $triggerPath; echo `$?'").Trim()
if ($before -ne "1") { throw "Action-window trigger already exists or is unreadable" }
& $AdbPath -s $Device shell "su -c 'touch $triggerPath && chmod 600 $triggerPath'"
if ($LASTEXITCODE -ne 0) { throw "Failed to create action-window trigger" }
$after = (& $AdbPath -s $Device shell "su -c 'test -e $triggerPath; echo `$?'").Trim()
if ($after -eq "0") {
    Write-Host "NATURAL_ACTION_WINDOW_TRIGGERED pid=$pidText path=$triggerPath state=visible"
} elseif ($after -eq "1") {
    $afterTracer = (& $AdbPath -s $Device shell "cat /proc/$pidText/status | grep '^TracerPid:'").Trim()
    if ($afterTracer -notmatch '^TracerPid:\s*([1-9][0-9]*)$') {
        throw "Trigger disappeared without an attached recorder: '$afterTracer'"
    }
    Write-Host "NATURAL_ACTION_WINDOW_TRIGGERED pid=$pidText path=$triggerPath state=consumed"
} else {
    throw "Action-window trigger state is unreadable: '$after'"
}
