# GUARDED NATURAL PRE-ROLL STEERING+BRAKE REPLAY. This file is not authorization.
# Anchor search is read-only; the recording's replay ticks begin after the committed match.

param(
    [Parameter(Mandatory=$true)][string]$RecordingPath,
    [Parameter(Mandatory=$true)][string]$SourceReportPath,
    [Parameter(Mandatory=$true)][string]$AnchorPath,
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ReplayReportOutputPath = "",
    [string]$SearchReportOutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$Gate10Validated,
    [switch]$BrakeSourceLiveValidated,
    [switch]$ActionWindowSourceLiveValidated,
    [switch]$ActionUntilReleaseSourceLiveValidated,
    [switch]$AcknowledgeRetriedRaceAlreadyRunning,
    [switch]$AcknowledgeReadOnlySearchUntilMatchOrTimeout,
    [switch]$AcknowledgeFiveFixedDeltaWritesAfterMatch,
    [switch]$AcknowledgeTenCombinedSteeringBrakePairWritesAfterMatch,
    [switch]$AcknowledgeUpToFivePhysicsCorrectionsAfterMatch,
    [switch]$AcknowledgeRecordingFrameCountDeltaWritesAfterMatch,
    [switch]$AcknowledgeTwiceFrameCountCombinedPairWritesAfterMatch,
    [switch]$AcknowledgeUpToFrameCountPhysicsCorrectionsAfterMatch,
    [switch]$AcknowledgeFirstFrameGuard,
    [switch]$AcknowledgeShortPtraceStallRisk,
    [switch]$AcknowledgeExtendedPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\staging\a9tas_hwbp_natural_preroll_brake_replay_v1"
$source = Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$header = Join-Path $root "src\natural_preroll_anchor_v1.h"
$syncVerifier = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$actionSyncVerifier = Join-Path $root "tools\synchronized_action_window_recording_v1.py"
$actionReleaseSyncVerifier = Join-Path $root "tools\synchronized_action_until_release_recording_v1.py"
$anchorTool = Join-Path $root "tools\natural_preroll_anchor_v1.py"
$actionAnchorVerifier = Join-Path $root "tools\verify_action_window_anchor_v1.py"
$replayVerifier = Join-Path $root "tools\aligned_brake_replay_v1.py"
$baseAlignmentVerifier = Join-Path $root "tools\aligned_tick_replay_v1.py"
$reportParser = Join-Path $root "tools\parse_unified_executor_report_v6.py"
$searchVerifier = Join-Path $root "tools\natural_preroll_search_report_v1.py"
$pins = @{
    $binary = "836ba11bfa181a845ffd0dea478ae8de955bc6568e6b135b2a6a2d423312b798"
    $source = "65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4"
    $header = "dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75"
    $syncVerifier = "85cffee892202948a3064f4d89845dac96267545d9769dd39a36c100d5b4b251"
    $actionSyncVerifier = "22047bd85181a479ab22d97f5f88a95daca67a953d356c1113b54a5dcc40434d"
    $actionReleaseSyncVerifier = "08361285e33d9d587406b37dcf97266d857b51cce9a1ec320f9f2a17dc657bba"
    $anchorTool = "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf"
    $actionAnchorVerifier = "5afbcba34c50995712aa13c48e38596cad6ab354c77a875c0509e29d20b48237"
    $replayVerifier = "f15d23825d736206bd3fca4e0fd4ea841c8d96665e8a6514bb379b110139146c"
    $baseAlignmentVerifier = "8b67002d442dbc003e5a3bd39754fa7b01410ef1c5e1663888e5c1c8aa045391"
    $reportParser = "7df6868dd8e02b0e0647ba84d35b13e27fcba643d7bb8732896d128c60372289"
    $searchVerifier = "38f658dfcac9f99e05c53b1c907378bd0adc29007eafe7b9d87c269bf2355c36"
}
$ack = "I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_natural_preroll_brake_replay_v1"

function Get-Sha([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 180000) { throw "TimeoutMs must be 1000..180000" }
foreach ($item in @($RecordingPath, $SourceReportPath, $AnchorPath) + $pins.Keys) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing artifact: $item" }
}
foreach ($item in $pins.Keys) {
    $actual = Get-Sha $item
    if ($actual -ne $pins[$item]) { throw "Reviewed artifact hash mismatch: $item expected=$($pins[$item]) actual=$actual" }
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$SourceReportPath = [IO.Path]::GetFullPath($SourceReportPath)
$AnchorPath = [IO.Path]::GetFullPath($AnchorPath)
$recordingBytes = [IO.File]::ReadAllBytes($RecordingPath)
if ($recordingBytes.Length -lt 24) { throw "A9UTK1 is shorter than its header" }
$recordingFrameCount = [BitConverter]::ToUInt32($recordingBytes, 20)
$sourceBytes = [IO.File]::ReadAllBytes($SourceReportPath)
if ($sourceBytes.Length -lt 8) { throw "Source report is shorter than its magic" }
$sourceMagic = [Text.Encoding]::ASCII.GetString($sourceBytes, 0, 8)
$isActionRelease = $sourceMagic.StartsWith("A9USR4")
$isActionWindow = $isActionRelease -or $sourceMagic.StartsWith("A9USR3")
$selectedSyncVerifier = if ($isActionRelease) { $actionReleaseSyncVerifier } elseif ($isActionWindow) { $actionSyncVerifier } else { $syncVerifier }
python -B $selectedSyncVerifier $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) { throw "Synchronized steering/brake source validation failed" }
python -B $anchorTool verify $AnchorPath $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) { throw "Bound brake A9NPA1 verification failed" }
if ($isActionWindow) {
    python -B $actionAnchorVerifier $SourceReportPath $RecordingPath $AnchorPath
    if ($LASTEXITCODE -ne 0) { throw "Neutral action-window anchor validation failed" }
}
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_synchronized_brake_recording_v1 test_synchronized_action_window_recording_v1 test_aligned_brake_replay_v1 test_natural_preroll_search_report_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Natural brake replay offline tests failed" }

if ($OfflineValidateOnly) {
    Write-Host "NATURAL_PREROLL_BRAKE_REPLAY_OFFLINE_VALIDATION_OK"
    Write-Host "source=$(if($isActionRelease){'A9USR4'}elseif($isActionWindow){'A9USR3'}else{'A9USR2'}) search_gameplay_writes=0 frames_after_match=$recordingFrameCount control_pair_writes=$($recordingFrameCount * 2)"
    Write-Host "binary_sha256=$($pins[$binary])"
    return
}

foreach ($gate in @(
    @($Gate10Validated, "Gate 10 live evidence"),
    @($AcknowledgeRetriedRaceAlreadyRunning, "an already-running retried race"),
    @($AcknowledgeReadOnlySearchUntilMatchOrTimeout, "read-only search until match or timeout"),
    @($AcknowledgeFirstFrameGuard, "the first-frame alignment guard")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if ($isActionWindow) {
    $sourceEvidenceGate = if ($isActionRelease) {
        @($ActionUntilReleaseSourceLiveValidated, "action-until-release source live evidence")
    } else {
        @($ActionWindowSourceLiveValidated, "action-window source live evidence")
    }
    foreach ($gate in @(
        $sourceEvidenceGate,
        @($AcknowledgeRecordingFrameCountDeltaWritesAfterMatch, "$recordingFrameCount fixed-delta writes after match"),
        @($AcknowledgeTwiceFrameCountCombinedPairWritesAfterMatch, "$($recordingFrameCount * 2) combined pair writes after match"),
        @($AcknowledgeUpToFrameCountPhysicsCorrectionsAfterMatch, "up to $recordingFrameCount physics corrections after match"),
        @($AcknowledgeExtendedPtraceStallRisk, "the extended ptrace stall risk")
    )) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
} else {
    foreach ($gate in @(
        @($BrakeSourceLiveValidated, "brake source live evidence"),
        @($AcknowledgeFiveFixedDeltaWritesAfterMatch, "five fixed-delta writes after match"),
        @($AcknowledgeTenCombinedSteeringBrakePairWritesAfterMatch, "ten combined steering/brake pair writes after match"),
        @($AcknowledgeUpToFivePhysicsCorrectionsAfterMatch, "up to five physics corrections after match"),
        @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
    )) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
}
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
if ($ReplayReportOutputPath -eq "") { $ReplayReportOutputPath = Join-Path $root "evidence\a9tas_natural_brake_replay_$stamp.a9uer6" }
if ($SearchReportOutputPath -eq "") { $SearchReportOutputPath = Join-Path $root "evidence\a9tas_natural_brake_replay_$stamp.a9npr1" }
$ReplayReportOutputPath = [IO.Path]::GetFullPath($ReplayReportOutputPath)
$SearchReportOutputPath = [IO.Path]::GetFullPath($SearchReportOutputPath)
if ($ReplayReportOutputPath -eq $SearchReportOutputPath) { throw "Output paths must differ" }
foreach ($output in @($ReplayReportOutputPath, $SearchReportOutputPath)) {
    if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
}
$remoteRecording = "/data/local/tmp/a9tas_natural_brake_replay_${gamePid}_$stamp.a9utk1"
$remoteAnchor = "/data/local/tmp/a9tas_natural_brake_replay_${gamePid}_$stamp.a9npa1"
$remoteReplay = "/data/local/tmp/a9tas_natural_brake_replay_${gamePid}_$stamp.a9uer6"
$remoteSearch = "/data/local/tmp/a9tas_natural_brake_replay_${gamePid}_$stamp.a9npr1"

function Get-RemoteSha([string]$Path) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $Path'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $line -notmatch '^([0-9a-fA-F]{64})\s+') { throw "Remote hash failed: $Path" }
    return $matches[1].ToLowerInvariant()
}
function Assert-CleanDetach {
    $after = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($after -ne $pidText) { throw "Game process changed during natural brake replay" }
    $afterTracer = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
    if ($afterTracer -ne "TracerPid:`t0" -and $afterTracer -ne "TracerPid: 0") { throw "Brake replay left a tracer: '$afterTracer'" }
}

foreach ($pair in @(
    @($binary, $remoteBinary),
    @($RecordingPath, $remoteRecording),
    @($AnchorPath, $remoteAnchor)
)) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair[0])" }
    if ((Get-RemoteSha $pair[1]) -ne (Get-Sha $pair[0])) { throw "Remote hash mismatch: $($pair[0])" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
Write-Host "NATURAL_PREROLL_BRAKE_REPLAY_START pid=$gamePid search_gameplay_writes=0"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteAnchor $remoteReplay $remoteSearch $ack 0 0 0'" | Out-Host
$executionCode = $LASTEXITCODE
Assert-CleanDetach
if ($executionCode -ne 0) { throw "Natural brake replay failed closed (exit=$executionCode, TracerPid=0)" }

$reportPairs = @(
    [pscustomobject]@{Remote=$remoteReplay; Local=$ReplayReportOutputPath}
    [pscustomobject]@{Remote=$remoteSearch; Local=$SearchReportOutputPath}
)
foreach ($pair in $reportPairs) {
    $remoteHash = Get-RemoteSha $pair.Remote
    & $AdbPath -s $Device shell "su -c 'chmod 644 $($pair.Remote)'"
    & $AdbPath -s $Device pull $pair.Remote $pair.Local | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $pair.Local) -ne $remoteHash) { throw "Pulled report mismatch: $($pair.Local)" }
}
python -B $searchVerifier $SearchReportOutputPath $AnchorPath
if ($LASTEXITCODE -ne 0) { throw "A9NPR1 search verification failed" }
python -B $replayVerifier $ReplayReportOutputPath $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) { throw "A9UER6 steering+brake replay verification failed" }

Write-Host "NATURAL_PREROLL_BRAKE_REPLAY_PASSED"
Write-Host "Replay report: $ReplayReportOutputPath"
Write-Host "Search report: $SearchReportOutputPath"
