# GUARDED BOUNDED ACTION-UNTIL-RELEASE CAPTURE. This file is not authorization.
# The neutral anchor is followed by a variable-length recording that succeeds
# only after simultaneous steering+brake, release, and 30 post-release frames.

param(
    [int]$TimeoutMs = 120000,
    [int]$FixedIntervalUs = 16667,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [string]$RawAnchorOutputPath = "",
    [string]$BoundAnchorOutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$Gate10Validated,
    [switch]$BrakeSourceLiveValidated,
    [switch]$AcknowledgeA9USR3TimingFailure,
    [switch]$AcknowledgeRaceAlreadyRunning,
    [switch]$AdbEscapeResumeImmediatelyBeforeAttach,
    [switch]$AcknowledgeSingleEscapeResumeInput,
    [switch]$AcknowledgeHostTriggerWaitHasZeroGameplayWrites,
    [switch]$AcknowledgeNeutralControlAnchor,
    [switch]$AcknowledgeUpTo3600FixedDeltaWrites,
    [switch]$AcknowledgeStopsOnlyAfterActionReleaseAnd30Frames,
    [switch]$AcknowledgeReadOnlySteeringBrakeCapture,
    [switch]$AcknowledgeNoControlOrPhysicsWrites,
    [switch]$AcknowledgeExtendedPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\staging\a9tas_hwbp_natural_action_until_release_recorder_v1"
$source = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$header = Join-Path $root "src\natural_preroll_anchor_v1.h"
$syncVerifier = Join-Path $root "tools\synchronized_action_until_release_recording_v1.py"
$anchorTool = Join-Path $root "tools\natural_preroll_anchor_v1.py"
$anchorVerifier = Join-Path $root "tools\verify_action_window_anchor_v1.py"
$pins = @{
    $binary = "e1630d2e539054a0b86438fab5c6bbe95c95989294070c6d68cef7824a378659"
    $source = "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2"
    $header = "dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75"
    $syncVerifier = "08361285e33d9d587406b37dcf97266d857b51cce9a1ec320f9f2a17dc657bba"
    $anchorTool = "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf"
    $anchorVerifier = "5afbcba34c50995712aa13c48e38596cad6ab354c77a875c0509e29d20b48237"
}
$ack = "I_ACCEPT_SYNC_ACTION_UNTIL_RELEASE_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_natural_action_until_release_recorder_v1"
$maximumFrames = 3600

function Get-Sha([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

if ($TimeoutMs -lt 10000 -or $TimeoutMs -gt 180000) { throw "TimeoutMs must be 10000..180000" }
if ($FixedIntervalUs -lt 1000 -or $FixedIntervalUs -gt 100000) { throw "FixedIntervalUs must be 1000..100000" }
foreach ($item in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing reviewed artifact: $item" }
    $actual = Get-Sha $item
    if ($actual -ne $pins[$item]) { throw "Reviewed artifact hash mismatch: $item expected=$($pins[$item]) actual=$actual" }
}
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_synchronized_action_until_release_recording_v1 test_natural_preroll_anchor_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Action-until-release source offline tests failed" }

if ($OfflineValidateOnly) {
    Write-Host "NATURAL_ACTION_UNTIL_RELEASE_SOURCE_OFFLINE_VALIDATION_OK"
    Write-Host "armed_gameplay_writes=0 anchor_control_pair=0 maximum_frames=3600 post_release_frames=30"
    Write-Host "capture=steering+brake-raw-bits writes=fixed-delta-only"
    Write-Host "binary_sha256=$($pins[$binary])"
    return
}

foreach ($gate in @(
    @($Gate10Validated, "Gate 10 live evidence"),
    @($BrakeSourceLiveValidated, "brake source live evidence"),
    @($AcknowledgeA9USR3TimingFailure, "the A9USR3 chat-timing failure"),
    @($AcknowledgeHostTriggerWaitHasZeroGameplayWrites, "zero gameplay writes while armed"),
    @($AcknowledgeNeutralControlAnchor, "an exact neutral steering/brake anchor"),
    @($AcknowledgeUpTo3600FixedDeltaWrites, "up to 3600 fixed-delta writes"),
    @($AcknowledgeStopsOnlyAfterActionReleaseAnd30Frames, "action, release, and 30 post-release frames"),
    @($AcknowledgeReadOnlySteeringBrakeCapture, "read-only steering/brake capture"),
    @($AcknowledgeNoControlOrPhysicsWrites, "zero control/physics writes"),
    @($AcknowledgeExtendedPtraceStallRisk, "the extended ptrace stall risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
$startModeCount = @(
    $AcknowledgeRaceAlreadyRunning,
    $AdbEscapeResumeImmediatelyBeforeAttach
).Where({ $_ }).Count
if ($startModeCount -ne 1) {
    throw "Choose exactly one start mode: already-running race or single-ESC resume immediately before attach."
}
if ($AdbEscapeResumeImmediatelyBeforeAttach -and
    -not $AcknowledgeSingleEscapeResumeInput) {
    throw "Single-ESC mode requires acknowledgement of exactly one resume input."
}
if (-not $AdbEscapeResumeImmediatelyBeforeAttach -and
    $AcknowledgeSingleEscapeResumeInput) {
    throw "Single-ESC acknowledgement is valid only in single-ESC mode."
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }

$deviceLine = & $AdbPath devices | Where-Object { $_ -match "^$([regex]::Escape($Device))\s+device$" }
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') { throw "Game PID unavailable: '$pidText'" }
$gamePid = [int]$pidText
$triggerPath = "/data/local/tmp/a9tas_action_window_start_$gamePid"
$triggerState = (& $AdbPath -s $Device shell "su -c 'test -e $triggerPath; echo `$?'").Trim()
if ($triggerState -ne "1") { throw "Stale or unreadable action trigger: $triggerPath state=$triggerState" }
$tracer = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracer -ne "TracerPid:`t0" -and $tracer -ne "TracerPid: 0") { throw "Game already traced: '$tracer'" }
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$bases = foreach ($line in $maps) {
    if ($line -match 'libAsphalt9\.so' -and
        $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
        [Convert]::ToUInt64($matches[2], 16) -eq 0) {
        [Convert]::ToUInt64($matches[1], 16)
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "Expected one zero-offset libAsphalt9 mapping, found $($bases.Count)" }
$baseHex = $bases[0].ToString("x")

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($RecordingOutputPath -eq "") { $RecordingOutputPath = Join-Path $root "evidence\a9tas_action_until_release_$stamp.a9utk1" }
if ($ReportOutputPath -eq "") { $ReportOutputPath = Join-Path $root "evidence\a9tas_action_until_release_$stamp.a9usr4" }
if ($RawAnchorOutputPath -eq "") { $RawAnchorOutputPath = Join-Path $root "evidence\a9tas_action_until_release_$stamp.raw.a9npa1" }
if ($BoundAnchorOutputPath -eq "") { $BoundAnchorOutputPath = Join-Path $root "evidence\a9tas_action_until_release_$stamp.a9npa1" }
$outputs = @($RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath, $BoundAnchorOutputPath) | ForEach-Object { [IO.Path]::GetFullPath($_) }
if (($outputs | Sort-Object -Unique).Count -ne 4) { throw "All output paths must differ" }
foreach ($output in $outputs) {
    if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
}
$RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath, $BoundAnchorOutputPath = $outputs
$remoteRecording = "/data/local/tmp/a9tas_action_until_release_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_action_until_release_${gamePid}_$stamp.a9usr4"
$remoteAnchor = "/data/local/tmp/a9tas_action_until_release_${gamePid}_$stamp.raw.a9npa1"

function Get-RemoteSha([string]$Path) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $Path'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $line -notmatch '^([0-9a-fA-F]{64})\s+') { throw "Remote hash failed: $Path" }
    return $matches[1].ToLowerInvariant()
}
function Assert-CleanDetach {
    $after = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($after -ne $pidText) { throw "Game process changed during action capture" }
    $afterTracer = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($afterTracer -ne "TracerPid:`t0" -and $afterTracer -ne "TracerPid: 0") { throw "Action recorder left a tracer: '$afterTracer'" }
    $remaining = (& $AdbPath -s $Device shell "su -c 'test -e $triggerPath; echo `$?'").Trim()
    if ($remaining -ne "1") { throw "Action trigger was not consumed: $triggerPath" }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Action-until-release binary push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ((Get-RemoteSha $remoteBinary) -ne $pins[$binary]) { throw "Remote binary hash mismatch" }
if ($AdbEscapeResumeImmediatelyBeforeAttach) {
    Write-Host "A9USR4_SINGLE_ESC_RESUME pid=$gamePid attach_started=0"
    & $AdbPath -s $Device shell input keyevent 111 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The single ESC resume input failed; recorder was not started" }
}
Write-Host "NATURAL_ACTION_UNTIL_RELEASE_ARMING pid=$gamePid trigger=$triggerPath wait_writes=0 maximum_frames=$maximumFrames"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $maximumFrames $FixedIntervalUs $remoteRecording $remoteReport $remoteAnchor $ack 0 0 0'" | Out-Host
$executionCode = $LASTEXITCODE
Assert-CleanDetach
if ($executionCode -ne 0) { throw "Action-until-release source failed closed (exit=$executionCode, TracerPid=0)" }

$remotePaths = @($remoteRecording, $remoteReport, $remoteAnchor)
$localPaths = @($RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath)
for ($index = 0; $index -lt 3; ++$index) {
    $remoteHash = Get-RemoteSha $remotePaths[$index]
    & $AdbPath -s $Device shell "su -c 'chmod 644 $($remotePaths[$index])'"
    & $AdbPath -s $Device pull $remotePaths[$index] $localPaths[$index] | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $localPaths[$index]) -ne $remoteHash) { throw "Pulled artifact mismatch: $($localPaths[$index])" }
}
python -B $syncVerifier $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9USR4/A9UTK1 action-until-release validation failed" }
python -B $anchorTool bind $RawAnchorOutputPath $ReportOutputPath $RecordingOutputPath $BoundAnchorOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9NPA1 A9USR4 source binding failed" }
python -B $anchorVerifier $ReportOutputPath $RecordingOutputPath $BoundAnchorOutputPath
if ($LASTEXITCODE -ne 0) { throw "Neutral A9USR4 anchor verification failed" }

Write-Host "NATURAL_ACTION_UNTIL_RELEASE_SOURCE_PASSED"
Write-Host "Recording: $RecordingOutputPath"
Write-Host "Report: $ReportOutputPath"
Write-Host "Raw anchor: $RawAnchorOutputPath"
Write-Host "Bound anchor: $BoundAnchorOutputPath"
