# GUARDED FIVE-FRAME ALIGNED NATURAL REPLAY. This file is not authorization.
# It accepts only an A9UTK1 recording bound to a successful A9USR1 capture.

param(
    [Parameter(Mandatory = $true)]
    [string]$RecordingPath,
    [Parameter(Mandatory = $true)]
    [string]$SourceReportPath,
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$Gate9Validated,
    [switch]$SynchronizedRecorderValidated,
    [switch]$AcknowledgeFreshAlignedStartCapture,
    [switch]$AcknowledgeRaceRunningBeforeAttach,
    [switch]$AcknowledgePausedAnchorThenSingleResume,
    [switch]$HostTriggeredRunningAttach,
    [switch]$AcknowledgeResumeImmediatelyBeforeAttach,
    [switch]$AdbEscapeResumeImmediatelyBeforeAttach,
    [switch]$AcknowledgeSingleEscapeResumeInput,
    [switch]$AcknowledgeFiveFixedDeltaWrites,
    [switch]$AcknowledgeTenSteeringWrites,
    [switch]$AcknowledgeUpToFiveTransformLinearCorrections,
    [switch]$AcknowledgeFirstFrameGuardBeforePoseWrite,
    [switch]$AcknowledgeFiveFrameGateNoTrajectoryClaim
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_aligned_tick_replay_v1"
$source = Join-Path $projectRoot "src\hwbp_unified_tick_executor_v1.cpp"
$sourceVerifier = Join-Path $projectRoot "tools\synchronized_tick_recording_v1.py"
$resultVerifier = Join-Path $projectRoot "tools\aligned_tick_replay_v1.py"
$expectedBinaryHash = "6b69c6cab9f517102b733edb55ea5530eac097966b05c0da412ed26bac655f9f"
$expectedSourceHash = "65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4"
$expectedSourceVerifierHash = "5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae"
$expectedResultVerifierHash = "8b67002d442dbc003e5a3bd39754fa7b01410ef1c5e1663888e5c1c8aa045391"
$acknowledgement = "I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_aligned_tick_replay_v1"

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
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') {
        throw "Invalid explicit address '$value'"
    }
}
foreach ($required in @(
    $binary, $source, $sourceVerifier, $resultVerifier,
    $RecordingPath, $SourceReportPath
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required local artifact is missing: $required"
    }
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$SourceReportPath = [IO.Path]::GetFullPath($SourceReportPath)
$actualBinaryHash = Get-LocalSha256 $binary
if ($actualBinaryHash -ne $expectedBinaryHash) {
    throw "Aligned replay binary hash mismatch"
}
if ((Get-LocalSha256 $source) -ne $expectedSourceHash) {
    throw "Aligned replay source differs from reviewed source"
}
if ((Get-LocalSha256 $sourceVerifier) -ne $expectedSourceVerifierHash) {
    throw "Synchronized source verifier differs from reviewed verifier"
}
if ((Get-LocalSha256 $resultVerifier) -ne $expectedResultVerifierHash) {
    throw "Aligned replay result verifier differs from reviewed verifier"
}

$sourceAssessment = @(
    & python -B $sourceVerifier $SourceReportPath $RecordingPath 2>&1
)
$sourceExitCode = $LASTEXITCODE
$sourceAssessment | Out-Host
if ($sourceExitCode -ne 0) {
    throw "A9USR1/A9UTK1 synchronized source validation failed"
}
$sourceText = $sourceAssessment -join [Environment]::NewLine
if ($sourceText -notmatch 'a9usr1_supported=1 frames=5 ticks=0\.\.4') {
    throw "The first aligned replay gate requires exactly five source frames"
}
$recordingHash = Get-LocalSha256 $RecordingPath
$sourceReportHash = Get-LocalSha256 $SourceReportPath

if ($OfflineValidateOnly) {
    Write-Host "ALIGNED_NATURAL_REPLAY_OFFLINE_VALIDATION_OK"
    Write-Host "frames=5 delta_writes=5 steering_writes=10 final_transactions=0..5"
    Write-Host "recording_sha256=$recordingHash"
    Write-Host "source_report_sha256=$sourceReportHash"
    Write-Host "executor_sha256=$actualBinaryHash"
    return
}

if ($AcknowledgePausedAnchorThenSingleResume) {
    throw "Paused-anchor live mode is quarantined after tombstone_45 (libhoudini SIGSEGV); use already-running attach only."
}

if (-not $Gate9Validated) {
    throw "Gate 9 steering+final evidence must be acknowledged."
}
if (-not $SynchronizedRecorderValidated) {
    throw "The synchronized recorder live gate must be acknowledged."
}
if (-not $AcknowledgeFreshAlignedStartCapture) {
    throw "The source must be a fresh capture from the same repeatable race start."
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
    throw "This replay writes the fixed delta on exactly five ticks."
}
if (-not $AcknowledgeTenSteeringWrites) {
    throw "This replay writes steering at C98 and C9C on five ticks."
}
if (-not $AcknowledgeUpToFiveTransformLinearCorrections) {
    throw "This replay may perform up to five complete 64+12 corrections."
}
if (-not $AcknowledgeFirstFrameGuardBeforePoseWrite) {
    throw "The first frame must pass the alignment guard before any pose write."
}
if (-not $AcknowledgeFiveFrameGateNoTrajectoryClaim) {
    throw "This five-frame gate is not yet a full trajectory claim."
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
if ($OutputPath -eq "") {
    $OutputPath = Join-Path $projectRoot `
        "evidence\a9tas_aligned_natural_replay_$stamp.a9uer5"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) {
    throw "Local report path already exists: $OutputPath"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$remoteRecording = "/data/local/tmp/a9tas_aligned_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_aligned_${gamePid}_$stamp.a9uer5"

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
        throw "Game process exited or changed PID during aligned replay"
    }
    $tracerPid = (& $AdbPath -s $Device shell `
        "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
        throw "Aligned replay left a tracer attached: '$tracerPid'"
    }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Aligned replay binary push failed" }
& $AdbPath -s $Device push $RecordingPath $remoteRecording | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9UTK1 source push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Aligned replay chmod failed" }
if ((Get-RemoteSha256 $remoteBinary) -ne $actualBinaryHash) {
    throw "Remote aligned replay binary hash differs after push"
}
if ((Get-RemoteSha256 $remoteRecording) -ne $recordingHash) {
    throw "Remote A9UTK1 hash differs after push"
}

Write-Host "ALIGNED_NATURAL_REPLAY_ARMING frames=5 pid=$gamePid first_guard=enabled"
if ($AcknowledgePausedAnchorThenSingleResume) {
    Write-Host "PAUSED_ANCHOR_ARMED: resume the retried race exactly once; do not Retry or restart."
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
    Write-Host "SINGLE_ESCAPE_SENT: beginning aligned replay attach now."
} else {
    Write-Host "The retried race must already be running; no input is required after this line."
}
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteReport $acknowledgement $PhysicsContextHex $MainObjectHex $FinalOwnerHex'" `
    | Out-Host
$executionCode = $LASTEXITCODE
Assert-StableGameAndDetach
if ($executionCode -ne 0) {
    throw "Aligned replay failed closed before acceptance (exit=$executionCode, TracerPid=0)"
}

$remoteReportHash = Get-RemoteSha256 $remoteReport
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'"
if ($LASTEXITCODE -ne 0) { throw "Could not make A9UER5 readable" }
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9UER5 report pull failed" }
$localReportHash = Get-LocalSha256 $OutputPath
if ($localReportHash -ne $remoteReportHash) {
    throw "Pulled A9UER5 hash differs from remote report"
}
python -B $resultVerifier $OutputPath $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) {
    throw "Aligned replay report/source acceptance failed"
}

Write-Host "ALIGNED_NATURAL_REPLAY_PASSED"
Write-Host "Recording SHA256 $recordingHash"
Write-Host "Source report SHA256 $sourceReportHash"
Write-Host "Replay report: $OutputPath"
Write-Host "Replay report SHA256 $localReportHash"
