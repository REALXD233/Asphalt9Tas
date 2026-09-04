# GUARDED start-line unified replay. This file is not authorization.
# Scope: fixed delta, recorded steering+brake, and conditional all-or-nothing
# transform+linear correction. Nitro/actions remain absent.

param(
    [Parameter(Mandatory=$true)][string]$RecordingPath,
    [Parameter(Mandatory=$true)][string]$RecordingSha256,
    [Parameter(Mandatory=$true)][string]$SourceReportPath,
    [Parameter(Mandatory=$true)][string]$SourceReportSha256,
    [int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$ExecuteExactlyOneLiveAttempt,
    [switch]$AcknowledgeStartlineSourceAndSameFixedEnvironment,
    [switch]$AcknowledgeReadyNoAttachBeforeResume,
    [switch]$AcknowledgeExactlyOneEscapeResumeInput,
    [switch]$AcknowledgeFrameCountFixedDeltaWrites,
    [switch]$AcknowledgeTwiceFrameCountControlPairWrites,
    [switch]$AcknowledgeUpToTwiceFrameCountPhysicsWrites,
    [switch]$AcknowledgeNoNitroOrGameplayActionCalls,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\startline-unified-replay-v1\a9tas_hwbp_startline_unified_replay_v1"
$wrapper = Join-Path $root "src\hwbp_startline_unified_replay_v1.cpp"
$core = Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$protocol = Join-Path $root "src\startline_prearm_protocol_v1.cpp"
$verifier = Join-Path $root "tools\aligned_brake_replay_v1.py"
$parser = Join-Path $root "tools\parse_unified_executor_report_v6.py"
$pins = @{
    $binary = "db6389110fbbbc9be53bd5d85065d963afb68151522b620ff5e78beb9085cb1e"
    $wrapper = "7c5a9eee62ab576d7f8be35ea0ec15e0e4ba64c5cd635c9623c8085aefe749d5"
    $core = "98d6392e40eb53e13afed9598ef7214ef57b4db1be0dacea528c2d737b84dc00"
    $protocol = "16345d7939a8b1e3ff8376828efe4bf2455375ee0fe28bf0179d821fccc56ac7"
    $verifier = "e900e426275cd2d2078e5800ea2acd24b4e69513b5d67b5cf8571145888d099e"
    $parser = "7df6868dd8e02b0e0647ba84d35b13e27fcba643d7bb8732896d128c60372289"
}
$ack = "I_ACCEPT_STARTLINE_UNIFIED_STEERING_BRAKE_PHYSICS_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_startline_unified_replay_v1"

function Get-Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }
function Invoke-AdbText([string[]]$Arguments) { return ((& $AdbPath @Arguments) | Out-String).Trim() }

if ($TimeoutMs -lt 10000 -or $TimeoutMs -gt 180000) { throw "TimeoutMs must be 10000..180000" }
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid address: $value" }
}
foreach ($item in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf) -or (Get-Sha $item) -ne $pins[$item]) {
        throw "Reviewed unified startline artifact hash mismatch: $item"
    }
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$SourceReportPath = [IO.Path]::GetFullPath($SourceReportPath)
foreach ($binding in @(@($RecordingPath, $RecordingSha256), @($SourceReportPath, $SourceReportSha256))) {
    $expected = $binding[1].ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$' -or
        -not (Test-Path -LiteralPath $binding[0] -PathType Leaf) -or
        (Get-Sha $binding[0]) -ne $expected) { throw "Source binding hash mismatch: $($binding[0])" }
}
$bytes = [IO.File]::ReadAllBytes($RecordingPath)
if ($bytes.Length -lt 96) { throw "Recording header is truncated" }
$frameCount = [BitConverter]::ToUInt32($bytes, 20)
if ($frameCount -lt 2 -or $frameCount -gt 3600 -or $bytes.Length -ne 96 + $frameCount * 144) {
    throw "Unified startline recording must contain 2..3600 exact frames"
}
if ([BitConverter]::ToUInt32($bytes, 24) -ne 16667) { throw "Recording fixed interval must be 16667" }
for ($index = 0; $index -lt $frameCount; ++$index) {
    $skip = [BitConverter]::ToUInt32($bytes, 96 + $index * 144 + 32)
    if ($skip -ne 0x7c) { throw "Frame $index must have exact unified scope 0x7c, got 0x$($skip.ToString('x'))" }
}
$pairCount = $frameCount * 2
$maximumPhysicsWrites = $frameCount * 2
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_startline_prearm_protocol_v1 test_startline_unified_replay_policy_v1 test_unified_brake_gate_v1 test_aligned_brake_replay_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Unified startline offline tests failed" }
if ($OfflineValidateOnly) {
    Write-Host "STARTLINE_UNIFIED_REPLAY_OFFLINE_VALIDATION_OK"
    Write-Host "frames=$frameCount delta=$frameCount control_pairs=$pairCount maximum_physics_writes=$maximumPhysicsWrites"
    Write-Host "alignment=first_complete_post_attach_cycle physical_anchor_search=0 nitro_action_calls=0"
    Write-Host "binary_sha256=$($pins[$binary]) recording_sha256=$($RecordingSha256.ToLowerInvariant()) source_report_sha256=$($SourceReportSha256.ToLowerInvariant())"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneLiveAttempt, "exactly one live attempt"),
    @($AcknowledgeStartlineSourceAndSameFixedEnvironment, "a startline source and identical Ancient Ruins/ZL1 environment"),
    @($AcknowledgeReadyNoAttachBeforeResume, "READY_NO_ATTACH before resume"),
    @($AcknowledgeExactlyOneEscapeResumeInput, "exactly one ESC resume input"),
    @($AcknowledgeFrameCountFixedDeltaWrites, "$frameCount fixed-delta writes"),
    @($AcknowledgeTwiceFrameCountControlPairWrites, "$pairCount control-pair writes"),
    @($AcknowledgeUpToTwiceFrameCountPhysicsWrites, "up to $maximumPhysicsWrites transform/linear writes"),
    @($AcknowledgeNoNitroOrGameplayActionCalls, "zero Nitro/gameplay action calls"),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, "ptrace stall, rollback and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
$deviceLines = (Invoke-AdbText @("devices")) -split "`r?`n"
if (-not ($deviceLines | Select-String -Quiet "^$([regex]::Escape($Device))\s+device$")) { throw "ADB device is not connected" }
$pidText = Invoke-AdbText @("-s", $Device, "shell", "su -c 'pidof $package'")
if ($pidText -notmatch '^\d+$') { throw "Game PID unavailable or ambiguous" }
$gamePid = [int]$pidText
$readyPath = "/data/local/tmp/a9tas_startline_ready_$gamePid"
if ((Invoke-AdbText @("-s", $Device, "shell", "su -c 'test -e $readyPath; echo `$?'")) -ne "1") { throw "Stale ready marker exists" }
function Get-Tracer { return Invoke-AdbText @("-s", $Device, "shell", "su -c 'grep ^TracerPid: /proc/$gamePid/status'") }
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Game is already traced" }
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$bases = @(foreach ($line in $maps) {
    if ($line -match 'libAsphalt9\.so' -and $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and [Convert]::ToUInt64($matches[2], 16) -eq 0) {
        [Convert]::ToUInt64($matches[1], 16)
    }
})
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base" }
$baseHex = $bases[0].ToString("x")
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($OutputPath -eq "") { $OutputPath = Join-Path $root "evidence\a9tas_startline_unified_${frameCount}f_$stamp.a9uer6" }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) { throw "Output already exists" }
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
$remoteRecording = "/data/local/tmp/a9tas_startline_unified_${frameCount}f_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_startline_unified_${gamePid}_$stamp.a9uer6"
foreach ($pair in @(@($binary, $remoteBinary, $pins[$binary]), @($RecordingPath, $remoteRecording, $RecordingSha256.ToLowerInvariant()))) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed" }
    $remoteHash = Invoke-AdbText @("-s", $Device, "shell", "su -c 'sha256sum $($pair[1])'")
    if ($remoteHash -notmatch '^([0-9a-fA-F]{64})\s+' -or $matches[1].ToLower() -ne $pair[2]) { throw "Remote hash mismatch" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'" | Out-Null
$remoteCommand = "$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteReport $ack $PhysicsContextHex $MainObjectHex $FinalOwnerHex"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath; $startInfo.UseShellExecute = $false; $startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true; $startInfo.RedirectStandardError = $true
foreach ($argument in @("-s", $Device, "shell", "su -c '$remoteCommand'")) { $startInfo.ArgumentList.Add($argument) }
$candidate = [Diagnostics.Process]::new(); $candidate.StartInfo = $startInfo
if (-not $candidate.Start()) { throw "Failed to launch resident unified replay" }
$ready = $false; $readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
while ([DateTime]::UtcNow -lt $readyDeadline -and -not $candidate.HasExited) {
    if ((Invoke-AdbText @("-s", $Device, "shell", "su -c 'test -e $readyPath; echo `$?'")) -eq "0") { $ready = $true; break }
    Start-Sleep -Milliseconds 100
}
if (-not $ready) { $candidate.WaitForExit(); throw "Unified candidate never published READY_NO_ATTACH: $($candidate.StandardError.ReadToEnd())" }
$payload = Invoke-AdbText @("-s", $Device, "shell", "su -c 'cat $readyPath'")
if ($payload -notmatch '^READY_NO_ATTACH_V1 ' -or $payload -notmatch 'target_threads_attached=0 game_writes=0' -or
    $payload -notmatch 'host_resume_gate=marker_removal' -or
    (Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Candidate attached before ESC or ready proof was invalid" }
Write-Host "STARTLINE_UNIFIED_HOST_READY pid=$gamePid frames=$frameCount TracerPid=0 sending_single_ESC=1"
& $AdbPath -s $Device shell input keyevent 111 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "The single ESC resume input failed" }
& $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Host resume acknowledgement failed" }
if (-not $candidate.WaitForExit($TimeoutMs + 15000)) { throw "Unified replay exceeded timeout; no retry was attempted" }
$stdout = $candidate.StandardOutput.ReadToEnd(); $stderr = $candidate.StandardError.ReadToEnd()
if ($stdout) { Write-Host $stdout.TrimEnd() }; if ($stderr) { Write-Warning $stderr.TrimEnd() }
$runCode = $candidate.ExitCode
if ((Invoke-AdbText @("-s", $Device, "shell", "su -c 'pidof $package'")) -ne $pidText) { throw "Game process changed" }
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Unified replay left a tracer" }
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Unified report pull failed" }
if ($runCode -ne 0) { throw "Unified replay failed closed (exit=$runCode); no retry was attempted" }
python -B $parser $OutputPath
if ($LASTEXITCODE -ne 0) { throw "A9UER6 structural validation failed" }
python -B $verifier --accept-startline-direct-audit-flag $OutputPath $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) { throw "Unified source/replay cross-validation failed" }
Write-Host "STARTLINE_UNIFIED_LIVE_PASSED frames=$frameCount delta=$frameCount pairs=$pairCount max_physics=$maximumPhysicsWrites single_ESC=1 TracerPid=0"
Write-Host "Report: $OutputPath"
