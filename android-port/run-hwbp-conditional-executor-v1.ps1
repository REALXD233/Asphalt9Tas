# GUARDED LIVE-WRITE RUNNER. Merely possessing this file does not authorize use.
# -OfflineValidateOnly performs local hash/ABI/frame-cap checks and never calls ADB.

param(
    [Parameter(Mandatory = $true)]
    [string]$RecordingPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedRecordingHash,
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$Gate2Validated,
    [switch]$SameBytesValidated,
    [switch]$AcknowledgeRacePausedThenResume,
    [switch]$AcknowledgeShortDifferentValueWrite,
    [switch]$AcknowledgeRollbackIsBestEffort
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_conditional_executor_v1"
$recordingParser = Join-Path $projectRoot `
    "tools\native_physics_recording_v1.py"
$reportParser = Join-Path $projectRoot `
    "tools\parse_conditional_audit_v1.py"
$expectedBinaryHash = `
    "57c74e137144f6d8a65d94caf35f2fc0d2a9ccf6520d6c77d5412399ba1e3660"
$acknowledgement = "I_ACCEPT_CONDITIONAL_A9NPS1_DIFFERENT_VALUES_V1"
$maximumLiveFrames = 10
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_conditional_executor_v1"

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

if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 30000) {
    throw "TimeoutMs must be between 1000 and 30000"
}
if ($PhysicsContextHex -notmatch '^(0x)?[0-9a-fA-F]+$') {
    throw "Invalid PhysicsContextHex '$PhysicsContextHex'"
}
if ($ExpectedRecordingHash -notmatch '^[0-9a-fA-F]{64}$') {
    throw "ExpectedRecordingHash must be exactly 64 hexadecimal characters"
}
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Conditional executor not built: $binary"
}
if (-not (Test-Path -LiteralPath $recordingParser)) {
    throw "A9NPS1 parser not found: $recordingParser"
}
if (-not (Test-Path -LiteralPath $reportParser)) {
    throw "A9CDT1 parser not found: $reportParser"
}
if (-not (Test-Path -LiteralPath $RecordingPath -PathType Leaf)) {
    throw "Recording not found: $RecordingPath"
}

$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$actualBinaryHash = Get-LocalSha256 $binary
if ($actualBinaryHash -ne $expectedBinaryHash) {
    throw "Executor hash mismatch: expected $expectedBinaryHash, got $actualBinaryHash"
}
$actualRecordingHash = Get-LocalSha256 $RecordingPath
if ($actualRecordingHash -ne $ExpectedRecordingHash.ToLowerInvariant()) {
    throw "Recording hash mismatch: expected $ExpectedRecordingHash, got $actualRecordingHash"
}

$recordingAssessment = @(
    & python -B $recordingParser $RecordingPath --require-runtime-safe 2>&1
)
$recordingExitCode = $LASTEXITCODE
$recordingAssessment | Out-Host
if ($recordingExitCode -ne 0) {
    throw "A9NPS1 runtime-safety validation failed"
}
$recordingText = $recordingAssessment -join [Environment]::NewLine
$frameMatch = [regex]::Match($recordingText, 'a9nps1_frames=(\d+)')
if (-not $frameMatch.Success) {
    throw "Could not recover the validated A9NPS1 frame count"
}
$recordingFrames = [int]$frameMatch.Groups[1].Value
if ($recordingFrames -lt 1 -or $recordingFrames -gt $maximumLiveFrames) {
    throw "Live conditional recording must contain 1..$maximumLiveFrames frames, got $recordingFrames"
}

if ($OfflineValidateOnly) {
    Write-Host "CONDITIONAL_RUNNER_OFFLINE_VALIDATION_OK"
    Write-Host "frames=$recordingFrames recording_sha256=$actualRecordingHash"
    Write-Host "executor_sha256=$actualBinaryHash"
    return
}

if (-not $Gate2Validated) {
    throw "Gate 2 live+Retry evidence must be acknowledged with -Gate2Validated."
}
if (-not $SameBytesValidated) {
    throw "The passed one-shot same-bytes gate must be acknowledged with -SameBytesValidated."
}
if (-not $AcknowledgeRacePausedThenResume) {
    throw "Start with the race paused, then resume only after the executor is armed. Pass -AcknowledgeRacePausedThenResume."
}
if (-not $AcknowledgeShortDifferentValueWrite) {
    throw "This may write different transform/linear values. Explicitly pass -AcknowledgeShortDifferentValueWrite."
}
if (-not $AcknowledgeRollbackIsBestEffort) {
    throw "Rollback is audited but cannot be guaranteed after a fatal transport failure. Pass -AcknowledgeRollbackIsBestEffort."
}
if (-not (Test-Path -LiteralPath $AdbPath)) {
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
if ($OutputPath -eq "") {
    $OutputPath = Join-Path $projectRoot `
        "evidence\a9tas_conditional_audit_$stamp.a9cdt1"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) {
    throw "Local report path already exists: $OutputPath"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$remoteRecording =
    "/data/local/tmp/a9tas_conditional_input_${gamePid}_$stamp.a9nps1"
$remoteReport =
    "/data/local/tmp/a9tas_conditional_audit_${gamePid}_$stamp.a9cdt1"

function Get-RemoteSha256([string]$RemotePath) {
    $hashLine = ((& $AdbPath -s $Device shell "sha256sum $RemotePath") |
        Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "Could not verify remote SHA256 for $RemotePath"
    }
    return $matches[1].ToLowerInvariant()
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Executor push failed" }
& $AdbPath -s $Device push $RecordingPath $remoteRecording | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recording push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Executor chmod failed" }
if ((Get-RemoteSha256 $remoteBinary) -ne $actualBinaryHash) {
    throw "Remote executor hash differs after push"
}
if ((Get-RemoteSha256 $remoteRecording) -ne $actualRecordingHash) {
    throw "Remote recording hash differs after push"
}

Write-Host "CONDITIONAL_EXECUTOR_ARMING frames=$recordingFrames pid=$gamePid"
Write-Host "Resume the paused race only after this line appears."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteReport $acknowledgement $PhysicsContextHex'" `
    | Out-Host
$executionCode = $LASTEXITCODE

$pidTextAfter = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidTextAfter -ne $pidText) {
    throw "Game process exited or changed PID during conditional execution"
}
$tracerPid = (& $AdbPath -s $Device shell `
    "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
    throw "Conditional executor did not detach cleanly: '$tracerPid'"
}
if ($executionCode -ne 0) {
    throw "Conditional executor failed closed (exit=$executionCode, TracerPid=0)"
}

$remoteReportHash = Get-RemoteSha256 $remoteReport
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9CDT1 report pull failed" }
$localReportHash = Get-LocalSha256 $OutputPath
if ($localReportHash -ne $remoteReportHash) {
    throw "Pulled report hash differs from remote report"
}
python -B $reportParser $OutputPath --require-supported `
    --minimum-corrected 1 --maximum-frames $maximumLiveFrames
if ($LASTEXITCODE -ne 0) { throw "A9CDT1 live acceptance failed" }

Write-Host "CONDITIONAL_EXECUTOR_LIVE_GATE_PASSED"
Write-Host "Audit report: $OutputPath"
Write-Host "SHA256 $localReportHash"
