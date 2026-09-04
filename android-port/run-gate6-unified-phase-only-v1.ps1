# GUARDED ONE-FRAME PHASE-ONLY LIVE RUNNER. This file is not authorization.
# OfflineValidateOnly verifies all local hashes/ABIs/policies and never calls ADB.
# The accepted A9UTK1 packet must skip all controls, actions and final correction.

param(
    [Parameter(Mandatory = $true)]
    [string]$RecordingPath,
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$Gate2Validated,
    [switch]$Gate5Validated,
    [switch]$AcknowledgeRaceRunningBeforeAttach,
    [switch]$AcknowledgeOneFixedDeltaWrite,
    [switch]$AcknowledgePhaseSwitchOnlyNoReplayClaim
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_unified_tick_executor_v1"
$inputVerifier = Join-Path $projectRoot `
    "tools\verify_unified_phase_only_gate_v1.py"
$resultVerifier = Join-Path $projectRoot `
    "tools\verify_gate6_phase_only_result_v1.py"
$expectedBinaryHash = `
    "e61aab11346c38f26fdecbafb2cf30394f69cda6462964f9c4fce3165534760f"
$acknowledgement = "I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_unified_tick_executor_v1"

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
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') {
        throw "Invalid explicit address '$value'"
    }
}
foreach ($required in @(
    $binary, $inputVerifier, $resultVerifier, $RecordingPath, $ManifestPath
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required local artifact is missing: $required"
    }
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$ManifestPath = [IO.Path]::GetFullPath($ManifestPath)
$actualBinaryHash = Get-LocalSha256 $binary
if ($actualBinaryHash -ne $expectedBinaryHash) {
    throw "Executor hash mismatch: expected $expectedBinaryHash, got $actualBinaryHash"
}
$inputAssessment = @(
    & python -B $inputVerifier $RecordingPath --manifest $ManifestPath 2>&1
)
$inputExitCode = $LASTEXITCODE
$inputAssessment | Out-Host
if ($inputExitCode -ne 0) {
    throw "Gate 6 A9UTK1/manifest validation failed"
}
$inputText = $inputAssessment -join [Environment]::NewLine
$hashMatch = [regex]::Match($inputText, 'sha256=([0-9a-f]{64})')
if (-not $hashMatch.Success) {
    throw "Could not recover validated Gate 6 input SHA256"
}
$actualRecordingHash = Get-LocalSha256 $RecordingPath
if ($actualRecordingHash -ne $hashMatch.Groups[1].Value) {
    throw "Gate 6 input hash changed after validation"
}

if ($OfflineValidateOnly) {
    Write-Host "GATE6_PHASE_ONLY_OFFLINE_VALIDATION_OK"
    Write-Host "frames=1 skip_flags=0xff gameplay_actions=0 final_writes=0 fixed_delta_writes=1"
    Write-Host "recording_sha256=$actualRecordingHash"
    Write-Host "executor_sha256=$actualBinaryHash"
    return
}

if (-not $Gate2Validated) {
    throw "Gate 2 live+Retry evidence must be acknowledged with -Gate2Validated."
}
if (-not $Gate5Validated) {
    throw "Gate 5 conditional live+Retry evidence must be acknowledged with -Gate5Validated."
}
if (-not $AcknowledgeRaceRunningBeforeAttach) {
    throw "The race must already be running before attach. Pass -AcknowledgeRaceRunningBeforeAttach."
}
if (-not $AcknowledgeOneFixedDeltaWrite) {
    throw "This gate writes one fixed delta. Pass -AcknowledgeOneFixedDeltaWrite."
}
if (-not $AcknowledgePhaseSwitchOnlyNoReplayClaim) {
    throw "This validates phase switching only, not replay fidelity. Pass -AcknowledgePhaseSwitchOnlyNoReplayClaim."
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
        "evidence\a9tas_gate6_phase_only_report_$stamp.a9uer5"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) {
    throw "Local report path already exists: $OutputPath"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$remoteRecording =
    "/data/local/tmp/a9tas_gate6_phase_only_${gamePid}_$stamp.a9utk1"
$remoteReport =
    "/data/local/tmp/a9tas_gate6_phase_only_${gamePid}_$stamp.a9uer5"

function Get-RemoteSha256([string]$RemotePath) {
    $hashLine = ((& $AdbPath -s $Device shell `
        "su -c 'sha256sum $RemotePath'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "Could not verify remote SHA256 for $RemotePath"
    }
    return $matches[1].ToLowerInvariant()
}

function Assert-StableGameAndDetach {
    $pidAfter = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($LASTEXITCODE -ne 0 -or $pidAfter -ne $pidText) {
        throw "Game process exited or changed PID during Gate 6"
    }
    $tracerPid = (& $AdbPath -s $Device shell `
        "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
        throw "Gate 6 left a tracer attached: '$tracerPid'"
    }
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

Write-Host "GATE6_PHASE_ONLY_ARMING frames=1 pid=$gamePid"
Write-Host "The race must already be running; no user input is required after this line."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteReport $acknowledgement $PhysicsContextHex $MainObjectHex $FinalOwnerHex'" `
    | Out-Host
$executionCode = $LASTEXITCODE
Assert-StableGameAndDetach
if ($executionCode -ne 0) {
    throw "Gate 6 executor failed closed (exit=$executionCode, TracerPid=0)"
}

$remoteReportHash = Get-RemoteSha256 $remoteReport
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'"
if ($LASTEXITCODE -ne 0) { throw "Could not make A9UER5 readable for pull" }
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9UER5 report pull failed" }
$localReportHash = Get-LocalSha256 $OutputPath
if ($localReportHash -ne $remoteReportHash) {
    throw "Pulled report hash differs from remote report"
}
python -B $resultVerifier $OutputPath $RecordingPath $ManifestPath
if ($LASTEXITCODE -ne 0) {
    throw "Gate 6 phase-only acceptance failed"
}

Write-Host "GATE6_UNIFIED_PHASE_ONLY_PASSED"
Write-Host "Input SHA256 $actualRecordingHash"
Write-Host "Report: $OutputPath"
Write-Host "Report SHA256 $localReportHash"
