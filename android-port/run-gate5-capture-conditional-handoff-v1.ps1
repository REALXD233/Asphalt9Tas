# GUARDED LIVE-WRITE ORCHESTRATOR. This file is not authorization to run it.
# It pre-stages both binaries, captures five read-only frames, then immediately
# hands that exact remote A9NPS1 to the conditional executor.

param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [string]$PhysicsContextHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$Gate2Validated,
    [switch]$SameBytesValidated,
    [switch]$AcknowledgeStartPausedThenResume,
    [switch]$AcknowledgeImmediateDifferentValueHandoff,
    [switch]$AcknowledgeRollbackIsBestEffort
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$recorder = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_native_physics_recorder_v1"
$conditional = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_conditional_executor_v1"
$recordingParser = Join-Path $projectRoot `
    "tools\native_physics_recording_v1.py"
$reportParser = Join-Path $projectRoot `
    "tools\parse_conditional_audit_v1.py"
$expectedRecorderHash = `
    "ba5748752644cdc8d6630f2f281532d5a80e8263f65bd2f09d38a5d257cc516a"
$expectedConditionalHash = `
    "57c74e137144f6d8a65d94caf35f2fc0d2a9ccf6520d6c77d5412399ba1e3660"
$conditionalAcknowledgement = `
    "I_ACCEPT_CONDITIONAL_A9NPS1_DIFFERENT_VALUES_V1"
$targetFrames = 5
$captureTimeoutMs = 120000
$conditionalTimeoutMs = 30000
$package = "com.aligames.kuang.kybc.aligames"
$remoteRecorder = "/data/local/tmp/a9tas_hwbp_native_physics_recorder_v1"
$remoteConditional = "/data/local/tmp/a9tas_hwbp_conditional_executor_v1"

function Get-LocalSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return -join ($sha256.ComputeHash($stream) | ForEach-Object {
                $_.ToString("x2")
            })
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

if ($PhysicsContextHex -notmatch '^(0x)?[0-9a-fA-F]+$') {
    throw "Invalid PhysicsContextHex '$PhysicsContextHex'"
}
foreach ($required in @($recorder, $conditional, $recordingParser, $reportParser)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required local artifact is missing: $required"
    }
}
$recorderHash = Get-LocalSha256 $recorder
$conditionalHash = Get-LocalSha256 $conditional
if ($recorderHash -ne $expectedRecorderHash) {
    throw "Recorder hash mismatch: expected $expectedRecorderHash, got $recorderHash"
}
if ($conditionalHash -ne $expectedConditionalHash) {
    throw "Conditional hash mismatch: expected $expectedConditionalHash, got $conditionalHash"
}

if ($OfflineValidateOnly) {
    Write-Host "GATE5_HANDOFF_OFFLINE_VALIDATION_OK"
    Write-Host "target_frames=$targetFrames"
    Write-Host "recorder_sha256=$recorderHash"
    Write-Host "conditional_sha256=$conditionalHash"
    return
}

if (-not $Gate2Validated) {
    throw "Gate 2 live+Retry evidence must be acknowledged with -Gate2Validated."
}
if (-not $SameBytesValidated) {
    throw "The passed same-bytes gate must be acknowledged with -SameBytesValidated."
}
if (-not $AcknowledgeStartPausedThenResume) {
    throw "Start paused and resume only after the recorder is armed. Pass -AcknowledgeStartPausedThenResume."
}
if (-not $AcknowledgeImmediateDifferentValueHandoff) {
    throw "The captured values will immediately feed a real conditional correction. Pass -AcknowledgeImmediateDifferentValueHandoff."
}
if (-not $AcknowledgeRollbackIsBestEffort) {
    throw "Rollback is audited but cannot survive every fatal transport failure. Pass -AcknowledgeRollbackIsBestEffort."
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
        "evidence\a9tas_gate5_handoff_input_$stamp.a9nps1"
}
if ($ReportOutputPath -eq "") {
    $ReportOutputPath = Join-Path $projectRoot `
        "evidence\a9tas_gate5_handoff_audit_$stamp.a9cdt1"
}
$RecordingOutputPath = [IO.Path]::GetFullPath($RecordingOutputPath)
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
foreach ($output in @($RecordingOutputPath, $ReportOutputPath)) {
    if (Test-Path -LiteralPath $output) {
        throw "Local output path already exists: $output"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) `
        -Force | Out-Null
}
$remoteRecording =
    "/data/local/tmp/a9tas_gate5_handoff_${gamePid}_$stamp.a9nps1"
$remoteReport =
    "/data/local/tmp/a9tas_gate5_handoff_${gamePid}_$stamp.a9cdt1"

function Get-RemoteSha256([string]$RemotePath) {
    $hashLine = ((& $AdbPath -s $Device shell `
        "su -c 'sha256sum $RemotePath'") |
        Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "Could not verify remote SHA256 for $RemotePath"
    }
    return $matches[1].ToLowerInvariant()
}

function Assert-StableGameAndDetach {
    $pidAfter = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($LASTEXITCODE -ne 0 -or $pidAfter -ne $pidText) {
        throw "Game process exited or changed PID during Gate 5 handoff"
    }
    $tracerPid = (& $AdbPath -s $Device shell `
        "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
        throw "Gate 5 handoff left a tracer attached: '$tracerPid'"
    }
}

& $AdbPath -s $Device push $recorder $remoteRecorder | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recorder push failed" }
& $AdbPath -s $Device push $conditional $remoteConditional | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Conditional executor push failed" }
& $AdbPath -s $Device shell `
    "su -c 'chmod 700 $remoteRecorder $remoteConditional'"
if ($LASTEXITCODE -ne 0) { throw "Remote chmod failed" }
if ((Get-RemoteSha256 $remoteRecorder) -ne $recorderHash) {
    throw "Remote recorder hash differs after push"
}
if ((Get-RemoteSha256 $remoteConditional) -ne $conditionalHash) {
    throw "Remote conditional hash differs after push"
}

Write-Host "GATE5_HANDOFF_RECORDER_ARMING frames=$targetFrames pid=$gamePid"
Write-Host "Resume the paused race only after the recorder start line appears."
& $AdbPath -s $Device shell `
    "su -c '$remoteRecorder $gamePid $baseHex $captureTimeoutMs $remoteRecording $PhysicsContextHex $targetFrames'" `
    | Out-Host
$recorderExitCode = $LASTEXITCODE
if ($recorderExitCode -ne 0) {
    Assert-StableGameAndDetach
    throw "Gate 5 read-only capture failed closed (exit=$recorderExitCode)"
}

Write-Host "GATE5_HANDOFF_CAPTURE_COMPLETE_STARTING_CONDITIONAL"
& $AdbPath -s $Device shell `
    "su -c '$remoteConditional $gamePid $baseHex $conditionalTimeoutMs $remoteRecording $remoteReport $conditionalAcknowledgement $PhysicsContextHex'" `
    | Out-Host
$conditionalExitCode = $LASTEXITCODE
Assert-StableGameAndDetach

$remoteRecordingHash = Get-RemoteSha256 $remoteRecording
& $AdbPath -s $Device pull $remoteRecording $RecordingOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Handoff recording pull failed" }
$localRecordingHash = Get-LocalSha256 $RecordingOutputPath
if ($localRecordingHash -ne $remoteRecordingHash) {
    throw "Pulled A9NPS1 hash differs from the executor input"
}
python -B $recordingParser $RecordingOutputPath --require-runtime-safe
if ($LASTEXITCODE -ne 0) { throw "Handoff A9NPS1 validation failed" }

if ($conditionalExitCode -ne 0) {
    throw "Conditional executor failed closed (exit=$conditionalExitCode, TracerPid=0)"
}
$remoteReportHash = Get-RemoteSha256 $remoteReport
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'"
if ($LASTEXITCODE -ne 0) { throw "Could not make A9CDT1 readable for pull" }
& $AdbPath -s $Device pull $remoteReport $ReportOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Handoff A9CDT1 pull failed" }
$localReportHash = Get-LocalSha256 $ReportOutputPath
if ($localReportHash -ne $remoteReportHash) {
    throw "Pulled A9CDT1 hash differs from the remote report"
}
python -B $reportParser $ReportOutputPath --require-supported `
    --minimum-corrected 1 --maximum-frames $targetFrames
if ($LASTEXITCODE -ne 0) { throw "Gate 5 handoff acceptance failed" }

Write-Host "GATE5_CAPTURE_CONDITIONAL_HANDOFF_PASSED"
Write-Host "Recording: $RecordingOutputPath"
Write-Host "Recording SHA256 $localRecordingHash"
Write-Host "Audit: $ReportOutputPath"
Write-Host "Audit SHA256 $localReportHash"
