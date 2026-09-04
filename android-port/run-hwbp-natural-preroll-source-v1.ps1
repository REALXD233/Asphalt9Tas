# GUARDED NATURAL PRE-ROLL SOURCE CAPTURE. This file is not authorization.
# Warmup is one complete read-only running cycle; the following five ticks are recorded.

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
    [switch]$Gate9Validated,
    [switch]$AcknowledgeRaceAlreadyRunning,
    [switch]$AcknowledgeOneReadOnlyWarmupCycle,
    [switch]$AcknowledgeFiveFixedDeltaWrites,
    [switch]$AcknowledgeNoControlOrPhysicsWrites,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot "build\staging\a9tas_hwbp_natural_preroll_recorder_v1"
$source = Join-Path $projectRoot "src\hwbp_synchronized_tick_recorder_v1.cpp"
$header = Join-Path $projectRoot "src\natural_preroll_anchor_v1.h"
$syncVerifier = Join-Path $projectRoot "tools\synchronized_tick_recording_v1.py"
$anchorTool = Join-Path $projectRoot "tools\natural_preroll_anchor_v1.py"
$expectedBinaryHash = "af414aec974b491a090c8ce70ae563651bb4fb2a0042b5f0849d5af3804d606c"
$expectedSourceHash = "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2"
$expectedHeaderHash = "dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75"
$expectedSyncVerifierHash = "5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae"
$expectedAnchorToolHash = "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf"
$acknowledgement = "I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_natural_preroll_recorder_v1"
$targetFrames = 5

function Get-LocalSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 30000) { throw "TimeoutMs must be 1000..30000" }
if ($FixedIntervalUs -lt 1000 -or $FixedIntervalUs -gt 100000) { throw "FixedIntervalUs must be 1000..100000" }
foreach ($item in @($binary, $source, $header, $syncVerifier, $anchorTool)) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing reviewed artifact: $item" }
}
$pins = @{
    $binary = $expectedBinaryHash; $source = $expectedSourceHash; $header = $expectedHeaderHash
    $syncVerifier = $expectedSyncVerifierHash; $anchorTool = $expectedAnchorToolHash
}
foreach ($item in $pins.Keys) {
    $actual = Get-LocalSha256 $item
    if ($actual -ne $pins[$item]) { throw "Reviewed artifact hash mismatch: $item expected=$($pins[$item]) actual=$actual" }
}

Push-Location (Join-Path $projectRoot "tools")
try {
    python -B -m unittest test_natural_preroll_anchor_v1 test_natural_preroll_recorder_policy_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Natural pre-roll source offline tests failed" }

if ($OfflineValidateOnly) {
    Write-Host "NATURAL_PREROLL_SOURCE_OFFLINE_VALIDATION_OK"
    Write-Host "warmup_cycles=1 warmup_gameplay_writes=0 recorded_frames=5"
    Write-Host "binary_sha256=$expectedBinaryHash"
    return
}

if (-not $Gate9Validated) { throw "Gate 9 evidence must be acknowledged" }
if (-not $AcknowledgeRaceAlreadyRunning) { throw "Natural pre-roll source requires an already-running race" }
if (-not $AcknowledgeOneReadOnlyWarmupCycle) { throw "Acknowledge one complete read-only warmup cycle" }
if (-not $AcknowledgeFiveFixedDeltaWrites) { throw "Acknowledge five fixed-delta writes after warmup" }
if (-not $AcknowledgeNoControlOrPhysicsWrites) { throw "Acknowledge zero control/physics writes" }
if (-not $AcknowledgeShortPtraceStallRisk) { throw "Acknowledge the short ptrace stall risk" }
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }

$deviceLine = & $AdbPath devices | Where-Object { $_ -match "^$([regex]::Escape($Device))\s+device$" }
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') { throw "Game PID is unavailable or ambiguous: '$pidText'" }
$gamePid = [int]$pidText
$tracer = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracer -ne "TracerPid:`t0" -and $tracer -ne "TracerPid: 0") { throw "Game already has a tracer: '$tracer'" }
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) { throw "Failed to read game maps" }
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
if ($RecordingOutputPath -eq "") { $RecordingOutputPath = Join-Path $projectRoot "evidence\a9tas_natural_source_$stamp.a9utk1" }
if ($ReportOutputPath -eq "") { $ReportOutputPath = Join-Path $projectRoot "evidence\a9tas_natural_source_$stamp.a9usr1" }
if ($RawAnchorOutputPath -eq "") { $RawAnchorOutputPath = Join-Path $projectRoot "evidence\a9tas_natural_source_$stamp.raw.a9npa1" }
if ($BoundAnchorOutputPath -eq "") { $BoundAnchorOutputPath = Join-Path $projectRoot "evidence\a9tas_natural_source_$stamp.a9npa1" }
$outputs = @($RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath, $BoundAnchorOutputPath) | ForEach-Object { [IO.Path]::GetFullPath($_) }
if (($outputs | Sort-Object -Unique).Count -ne 4) { throw "All four output paths must differ" }
foreach ($output in $outputs) {
    if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
}
$RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath, $BoundAnchorOutputPath = $outputs
$remoteRecording = "/data/local/tmp/a9tas_natural_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_natural_${gamePid}_$stamp.a9usr1"
$remoteAnchor = "/data/local/tmp/a9tas_natural_${gamePid}_$stamp.raw.a9npa1"

function Get-RemoteSha256([string]$Path) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $Path'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $line -notmatch '^([0-9a-fA-F]{64})\s+') { throw "Remote hash failed: $Path" }
    return $matches[1].ToLowerInvariant()
}
function Assert-CleanDetach {
    $after = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($after -ne $pidText) { throw "Game process changed during natural source capture" }
    $afterTracer = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($afterTracer -ne "TracerPid:`t0" -and $afterTracer -ne "TracerPid: 0") { throw "Natural source recorder left a tracer: '$afterTracer'" }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Natural source binary push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ((Get-RemoteSha256 $remoteBinary) -ne $expectedBinaryHash) { throw "Remote binary hash mismatch" }
Write-Host "NATURAL_PREROLL_SOURCE_START pid=$gamePid warmup_writes=0 recorded_frames=5"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $targetFrames $FixedIntervalUs $remoteRecording $remoteReport $remoteAnchor $acknowledgement 0 0 0'" | Out-Host
$executionCode = $LASTEXITCODE
Assert-CleanDetach
if ($executionCode -ne 0) { throw "Natural source recorder failed closed (exit=$executionCode, TracerPid=0)" }

$remotePaths = @($remoteRecording, $remoteReport, $remoteAnchor)
$localPaths = @($RecordingOutputPath, $ReportOutputPath, $RawAnchorOutputPath)
for ($index = 0; $index -lt 3; ++$index) {
    $remoteHash = Get-RemoteSha256 $remotePaths[$index]
    & $AdbPath -s $Device shell "su -c 'chmod 644 $($remotePaths[$index])'"
    & $AdbPath -s $Device pull $remotePaths[$index] $localPaths[$index] | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-LocalSha256 $localPaths[$index]) -ne $remoteHash) { throw "Pulled artifact mismatch: $($localPaths[$index])" }
}
python -B $syncVerifier $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "Natural source A9USR1/A9UTK1 validation failed" }
python -B $anchorTool bind $RawAnchorOutputPath $ReportOutputPath $RecordingOutputPath $BoundAnchorOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9NPA1 source binding failed" }
python -B $anchorTool verify $BoundAnchorOutputPath $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "Bound A9NPA1 verification failed" }

Write-Host "NATURAL_PREROLL_SOURCE_PASSED"
Write-Host "Recording: $RecordingOutputPath"
Write-Host "Report: $ReportOutputPath"
Write-Host "Raw anchor: $RawAnchorOutputPath"
Write-Host "Bound anchor: $BoundAnchorOutputPath"
