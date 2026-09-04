# GUARDED NATURAL PRE-ROLL BRAKE SOURCE CAPTURE. This file is not authorization.
# Complete cycles remain read-only until a natural brake press; five following
# ticks write only fixed delta while capturing steering/brake/physics.

param(
    [int]$TimeoutMs = 30000,
    [int]$FixedIntervalUs = 16667,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [string]$RawAnchorOutputPath = "",
    [string]$BoundAnchorOutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$Gate10Validated,
    [switch]$AcknowledgeRaceAlreadyRunning,
    [switch]$AcknowledgeReadOnlyWaitUntilBrakePress,
    [switch]$AcknowledgeFiveFixedDeltaWrites,
    [switch]$AcknowledgeReadOnlySteeringBrakeCapture,
    [switch]$AcknowledgeNoControlOrPhysicsWrites,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\staging\a9tas_hwbp_natural_preroll_brake_recorder_v1"
$source = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$header = Join-Path $root "src\natural_preroll_anchor_v1.h"
$syncVerifier = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$anchorTool = Join-Path $root "tools\natural_preroll_anchor_v1.py"
$pins = @{
    $binary = "e2382d280dd46aece092c3a042adc6b1042d21a047d7fbe66b0a294433032614"
    $source = "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2"
    $header = "dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75"
    $syncVerifier = "85cffee892202948a3064f4d89845dac96267545d9769dd39a36c100d5b4b251"
    $anchorTool = "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf"
}
$ack = "I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_BRAKE_CAPTURE_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_natural_preroll_brake_recorder_v1"
$targetFrames = 5

function Get-Sha([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 30000) { throw "TimeoutMs must be 1000..30000" }
if ($FixedIntervalUs -lt 1000 -or $FixedIntervalUs -gt 100000) { throw "FixedIntervalUs must be 1000..100000" }
foreach ($item in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing reviewed artifact: $item" }
    $actual = Get-Sha $item
    if ($actual -ne $pins[$item]) { throw "Reviewed artifact hash mismatch: $item expected=$($pins[$item]) actual=$actual" }
}
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_synchronized_brake_recording_v1 test_natural_preroll_anchor_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Natural brake source offline tests failed" }

if ($OfflineValidateOnly) {
    Write-Host "NATURAL_PREROLL_BRAKE_SOURCE_OFFLINE_VALIDATION_OK"
    Write-Host "wait_until_brake_le_minus_0_5_gameplay_writes=0 recorded_frames=5"
    Write-Host "capture=steering+brake-raw-bits writes=fixed-delta-only"
    Write-Host "binary_sha256=$($pins[$binary])"
    return
}

foreach ($gate in @(
    @($Gate10Validated, "Gate 10 live evidence"),
    @($AcknowledgeRaceAlreadyRunning, "an already-running race"),
    @($AcknowledgeReadOnlyWaitUntilBrakePress, "read-only wait until a natural brake press"),
    @($AcknowledgeFiveFixedDeltaWrites, "five fixed-delta writes"),
    @($AcknowledgeReadOnlySteeringBrakeCapture, "read-only steering/brake capture"),
    @($AcknowledgeNoControlOrPhysicsWrites, "zero control/physics writes"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }

$deviceLine = & $AdbPath devices | Where-Object { $_ -match "^$([regex]::Escape($Device))\s+device$" }
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') { throw "Game PID unavailable: '$pidText'" }
$gamePid = [int]$pidText
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
if ($RecordingOutputPath -eq "") { $RecordingOutputPath = Join-Path $root "evidence\a9tas_natural_brake_source_$stamp.a9utk1" }
if ($ReportOutputPath -eq "") { $ReportOutputPath = Join-Path $root "evidence\a9tas_natural_brake_source_$stamp.a9usr2" }
if ($RawAnchorOutputPath -eq "") { $RawAnchorOutputPath = Join-Path $root "evidence\a9tas_natural_brake_source_$stamp.raw.a9npa1" }
if ($BoundAnchorOutputPath -eq "") { $BoundAnchorOutputPath = Join-Path $root "evidence\a9tas_natural_brake_source_$stamp.a9npa1" }
$outputs = @($RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath, $BoundAnchorOutputPath) | ForEach-Object { [IO.Path]::GetFullPath($_) }
if (($outputs | Sort-Object -Unique).Count -ne 4) { throw "All output paths must differ" }
foreach ($output in $outputs) {
    if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
}
$RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath, $BoundAnchorOutputPath = $outputs
$remoteRecording = "/data/local/tmp/a9tas_natural_brake_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_natural_brake_${gamePid}_$stamp.a9usr2"
$remoteAnchor = "/data/local/tmp/a9tas_natural_brake_${gamePid}_$stamp.raw.a9npa1"

function Get-RemoteSha([string]$Path) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $Path'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $line -notmatch '^([0-9a-fA-F]{64})\s+') { throw "Remote hash failed: $Path" }
    return $matches[1].ToLowerInvariant()
}
function Assert-CleanDetach {
    $after = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($after -ne $pidText) { throw "Game process changed during natural brake source capture" }
    $afterTracer = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($afterTracer -ne "TracerPid:`t0" -and $afterTracer -ne "TracerPid: 0") { throw "Brake recorder left a tracer: '$afterTracer'" }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Natural brake source binary push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ((Get-RemoteSha $remoteBinary) -ne $pins[$binary]) { throw "Remote binary hash mismatch" }
Write-Host "NATURAL_PREROLL_BRAKE_SOURCE_START pid=$gamePid wait_writes=0 trigger=brake_le_-0.5 recorded_frames=5"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $targetFrames $FixedIntervalUs $remoteRecording $remoteReport $remoteAnchor $ack 0 0 0'" | Out-Host
$executionCode = $LASTEXITCODE
Assert-CleanDetach
if ($executionCode -ne 0) { throw "Natural brake source failed closed (exit=$executionCode, TracerPid=0)" }

$remotePaths = @($remoteRecording, $remoteReport, $remoteAnchor)
$localPaths = @($RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath)
for ($index = 0; $index -lt 3; ++$index) {
    $remoteHash = Get-RemoteSha $remotePaths[$index]
    & $AdbPath -s $Device shell "su -c 'chmod 644 $($remotePaths[$index])'"
    & $AdbPath -s $Device pull $remotePaths[$index] $localPaths[$index] | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $localPaths[$index]) -ne $remoteHash) { throw "Pulled artifact mismatch: $($localPaths[$index])" }
}
python -B $syncVerifier $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9USR2/A9UTK1 brake source validation failed" }
python -B $anchorTool bind $RawAnchorOutputPath $ReportOutputPath $RecordingOutputPath $BoundAnchorOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9NPA1 brake source binding failed" }
python -B $anchorTool verify $BoundAnchorOutputPath $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "Bound brake A9NPA1 verification failed" }

Write-Host "NATURAL_PREROLL_BRAKE_SOURCE_PASSED"
Write-Host "Recording: $RecordingOutputPath"
Write-Host "Report: $ReportOutputPath"
Write-Host "Raw anchor: $RawAnchorOutputPath"
Write-Host "Bound anchor: $BoundAnchorOutputPath"
