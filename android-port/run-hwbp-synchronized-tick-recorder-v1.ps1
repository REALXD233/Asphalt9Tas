# GUARDED FIVE-FRAME SYNCHRONIZED RECORDER. This file is not authorization.
# OfflineValidateOnly checks the reviewed binary/source/parser without ADB.

param(
    [int]$TimeoutMs = 30000,
    [int]$FixedIntervalUs = 16667,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$Gate9Validated,
    [switch]$AcknowledgeRaceRunningBeforeAttach,
    [switch]$AcknowledgePausedAnchorThenSingleResume,
    [switch]$HostTriggeredRunningAttach,
    [switch]$AcknowledgeResumeImmediatelyBeforeAttach,
    [switch]$AdbEscapeResumeImmediatelyBeforeAttach,
    [switch]$AcknowledgeSingleEscapeResumeInput,
    [switch]$AcknowledgeFiveFixedDeltaWrites,
    [switch]$AcknowledgeCaptureOnlyNoControlOrPhysicsWrites,
    [switch]$AcknowledgeFiveFrameCaptureCanStallGame,
    [switch]$AcknowledgeNaturalSameRaceFrames
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binaryName = if ($AcknowledgePausedAnchorThenSingleResume) {
    "a9tas_hwbp_paused_anchor_recorder_v1"
} else {
    "a9tas_hwbp_synchronized_tick_recorder_v1"
}
$binary = Join-Path $projectRoot "build\staging\$binaryName"
$source = Join-Path $projectRoot "src\hwbp_synchronized_tick_recorder_v1.cpp"
$verifier = Join-Path $projectRoot "tools\synchronized_tick_recording_v1.py"
$expectedBinaryHash = if ($AcknowledgePausedAnchorThenSingleResume) {
    "976b5c73c3de2544c7b79dfba33cf759f8b10af53b287da23e0decab43b676e8"
} else {
    "30bc9b0d8b16a372e3e85eddd8d8a51bd1eee684b7bc7fb04bc80fca58e12a93"
}
$expectedSourceHash = "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2"
$expectedVerifierHash = "5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae"
$acknowledgement = "I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/$binaryName"
$targetFrames = 5

function Get-LocalSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return -join ($sha256.ComputeHash($stream) | ForEach-Object {
                $_.ToString("x2")
            })
        } finally { $sha256.Dispose() }
    } finally { $stream.Dispose() }
}

$maximumTimeoutMs = if ($AcknowledgePausedAnchorThenSingleResume) { 120000 } else { 30000 }
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt $maximumTimeoutMs) {
    throw "TimeoutMs must be between 1000 and $maximumTimeoutMs for this mode"
}
if ($FixedIntervalUs -lt 1000 -or $FixedIntervalUs -gt 100000) {
    throw "FixedIntervalUs must be between 1000 and 100000"
}
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') {
        throw "Invalid explicit address '$value'"
    }
}
foreach ($required in @($binary, $source, $verifier)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required reviewed artifact is missing: $required"
    }
}
$actualBinaryHash = Get-LocalSha256 $binary
if ($actualBinaryHash -ne $expectedBinaryHash) {
    throw "Recorder hash mismatch: expected $expectedBinaryHash, got $actualBinaryHash"
}
if ((Get-LocalSha256 $source) -ne $expectedSourceHash) {
    throw "Synchronized recorder source differs from the reviewed source"
}
if ((Get-LocalSha256 $verifier) -ne $expectedVerifierHash) {
    throw "Synchronized recorder verifier differs from the reviewed verifier"
}

$toolsDirectory = Join-Path $projectRoot "tools"
Push-Location $toolsDirectory
try {
    $testOutput = @(
        & python -B -m unittest `
            test_synchronized_tick_recording_v1 `
            test_synchronized_tick_recorder_policy_v1 2>&1
    )
    $testExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
$testOutput | Out-Host
if ($testExitCode -ne 0) {
    throw "Synchronized recorder offline policy/codec tests failed"
}

if ($OfflineValidateOnly) {
    Write-Host "SYNCHRONIZED_TICK_RECORDER_OFFLINE_VALIDATION_OK"
    Write-Host "frames=5 writes=fixed-delta-only captured=steering+transform+linear"
    Write-Host "recorder_sha256=$actualBinaryHash"
    return
}

if ($AcknowledgePausedAnchorThenSingleResume) {
    throw "Paused-anchor live mode is quarantined after tombstone_45 (libhoudini SIGSEGV); use already-running attach only."
}

if (-not $Gate9Validated) {
    throw "Gate 9 steering+final evidence must be acknowledged."
}
$startModeCount = @(
    $AcknowledgeRaceRunningBeforeAttach,
    $AcknowledgePausedAnchorThenSingleResume,
    $HostTriggeredRunningAttach,
    $AdbEscapeResumeImmediatelyBeforeAttach
).Where({ $_ }).Count
if ($startModeCount -ne 1) {
    throw "Choose exactly one start mode: already running, quarantined paused anchor, host-triggered attach, or single-ESC attach."
}
if ($HostTriggeredRunningAttach -and
    -not $AcknowledgeResumeImmediatelyBeforeAttach) {
    throw "Host-trigger mode requires acknowledgement that resume happens immediately before attach."
}
if ($AdbEscapeResumeImmediatelyBeforeAttach -and
    -not $AcknowledgeSingleEscapeResumeInput) {
    throw "Single-ESC mode requires explicit acknowledgement of one resume input."
}
if (-not $AdbEscapeResumeImmediatelyBeforeAttach -and
    $AcknowledgeSingleEscapeResumeInput) {
    throw "Single-ESC acknowledgement is only valid with single-ESC mode."
}
if (-not $AcknowledgeFiveFixedDeltaWrites) {
    throw "This capture writes the fixed delta on exactly five ticks."
}
if (-not $AcknowledgeCaptureOnlyNoControlOrPhysicsWrites) {
    throw "Acknowledge that this run captures steering/physics but does not write them."
}
if (-not $AcknowledgeFiveFrameCaptureCanStallGame) {
    throw "Acknowledge that HWBP capture can briefly stall the game."
}
if (-not $AcknowledgeNaturalSameRaceFrames) {
    throw "The five frames must be natural, consecutive frames from one race."
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

$deviceLine = & $AdbPath devices | Where-Object {
    $_ -match "^$([regex]::Escape($Device))\s+device$"
}
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') {
    throw "Game process is not running or pidof was ambiguous: '$pidText'"
}
$gamePid = [int]$pidText
$tracerBefore = (& $AdbPath -s $Device shell `
    "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracerBefore -ne "TracerPid:`t0" -and $tracerBefore -ne "TracerPid: 0") {
    throw "Game already has a tracer: '$tracerBefore'"
}
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) { throw "Failed to read game maps" }
$bases = foreach ($line in $maps) {
    if ($line -notmatch 'libAsphalt9\.so') { continue }
    if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
        $start = [Convert]::ToUInt64($matches[1], 16)
        $offset = [Convert]::ToUInt64($matches[2], 16)
        if ($offset -eq 0) { $start }
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) {
    throw "Expected one zero-offset libAsphalt9 mapping, found $($bases.Count)"
}
$baseHex = $bases[0].ToString("x")

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($RecordingOutputPath -eq "") {
    $RecordingOutputPath = Join-Path $projectRoot `
        "evidence\a9tas_synchronized_tick_$stamp.a9utk1"
}
if ($ReportOutputPath -eq "") {
    $ReportOutputPath = Join-Path $projectRoot `
        "evidence\a9tas_synchronized_tick_report_$stamp.a9usr1"
}
$RecordingOutputPath = [IO.Path]::GetFullPath($RecordingOutputPath)
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
if ($RecordingOutputPath -eq $ReportOutputPath) {
    throw "RecordingOutputPath and ReportOutputPath must differ"
}
foreach ($output in @($RecordingOutputPath, $ReportOutputPath)) {
    if (Test-Path -LiteralPath $output) {
        throw "Local output path already exists: $output"
    }
}
foreach ($directory in @(
    (Split-Path -Parent $RecordingOutputPath),
    (Split-Path -Parent $ReportOutputPath)
) | Sort-Object -Unique) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$remoteRecording = "/data/local/tmp/a9tas_sync_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_sync_${gamePid}_$stamp.a9usr1"

function Get-RemoteSha256([string]$RemotePath) {
    $hashLine = ((& $AdbPath -s $Device shell `
        "su -c 'sha256sum $RemotePath'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "Could not verify remote SHA256 for $RemotePath"
    }
    return $matches[1].ToLowerInvariant()
}

function Assert-StableGameAndDetach {
    $pidAfter = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($LASTEXITCODE -ne 0 -or $pidAfter -ne $pidText) {
        throw "Game process exited or changed PID during synchronized capture"
    }
    $tracerPid = (& $AdbPath -s $Device shell `
        "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
        throw "Synchronized recorder left a tracer attached: '$tracerPid'"
    }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recorder push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Recorder chmod failed" }
if ((Get-RemoteSha256 $remoteBinary) -ne $actualBinaryHash) {
    throw "Remote recorder hash differs after push"
}

Write-Host "SYNCHRONIZED_TICK_RECORDER_ARMING frames=5 pid=$gamePid writes=fixed-delta-only"
if ($AcknowledgePausedAnchorThenSingleResume) {
    Write-Host "PAUSED_ANCHOR_ARMED: resume the same race exactly once; do not Retry or restart."
} elseif ($HostTriggeredRunningAttach) {
    Write-Host "HOST_TRIGGER_READY_NO_PTRACE: resume once, then send START."
    $trigger = [Console]::ReadLine()
    if ($trigger -ne "START") {
        throw "Host trigger must be exactly START; no tracer was attached."
    }
    Assert-StableGameAndDetach
    Write-Host "HOST_TRIGGER_ACCEPTED: beginning already-running attach now."
} elseif ($AdbEscapeResumeImmediatelyBeforeAttach) {
    Write-Host "SINGLE_ESCAPE_READY_NO_PTRACE: sending exactly one KEYCODE_ESCAPE, then attaching immediately."
    & $AdbPath -s $Device shell input keyevent 111
    if ($LASTEXITCODE -ne 0) {
        throw "The single KEYCODE_ESCAPE resume input failed; no tracer was attached."
    }
    Assert-StableGameAndDetach
    Write-Host "SINGLE_ESCAPE_SENT: beginning already-running attach now."
} else {
    Write-Host "The race must already be running; no user input is required after this line."
}
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $targetFrames $FixedIntervalUs $remoteRecording $remoteReport $acknowledgement $PhysicsContextHex $MainObjectHex $FinalOwnerHex'" `
    | Out-Host
$executionCode = $LASTEXITCODE
Assert-StableGameAndDetach
if ($executionCode -ne 0) {
    throw "Synchronized recorder failed closed (exit=$executionCode, TracerPid=0)"
}

$remoteRecordingHash = Get-RemoteSha256 $remoteRecording
$remoteReportHash = Get-RemoteSha256 $remoteReport
foreach ($remote in @($remoteRecording, $remoteReport)) {
    & $AdbPath -s $Device shell "su -c 'chmod 644 $remote'"
    if ($LASTEXITCODE -ne 0) { throw "Could not make output readable: $remote" }
}
& $AdbPath -s $Device pull $remoteRecording $RecordingOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9UTK1 recording pull failed" }
& $AdbPath -s $Device pull $remoteReport $ReportOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9USR1 report pull failed" }
$localRecordingHash = Get-LocalSha256 $RecordingOutputPath
$localReportHash = Get-LocalSha256 $ReportOutputPath
if ($localRecordingHash -ne $remoteRecordingHash -or
    $localReportHash -ne $remoteReportHash) {
    throw "Pulled synchronized output hash differs from the remote output"
}
python -B $verifier $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) {
    throw "Synchronized A9USR1/A9UTK1 cross-validation failed"
}

Write-Host "SYNCHRONIZED_TICK_RECORDER_PASSED"
Write-Host "Recording: $RecordingOutputPath"
Write-Host "Recording SHA256 $localRecordingHash"
Write-Host "Report: $ReportOutputPath"
Write-Host "Report SHA256 $localReportHash"
