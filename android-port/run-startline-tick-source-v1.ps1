# GUARDED start-line tick-zero source capture. This file is not authorization.
# The candidate is fully resident and read-only while countdown 3 is paused.
# The host waits for READY_NO_ATTACH, verifies TracerPid=0, sends exactly one
# ESC, and only then may the candidate attach and capture complete cycles.

param(
    [int]$TimeoutMs = 120000,
    [int]$TargetFrames = 900,
    [int]$FixedIntervalUs = 16667,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$ExecuteExactlyOneLiveAttempt,
    [switch]$AcknowledgeCountdown3PausedFixedEnvironment,
    [switch]$AcknowledgeReadyNoAttachBeforeResume,
    [switch]$AcknowledgeExactlyOneEscapeResumeInput,
    [switch]$AcknowledgeUpToTargetFrameFixedDeltaWrites,
    [switch]$AcknowledgeReadOnlySteeringBrakePhysicsCapture,
    [switch]$AcknowledgeNoControlOrPhysicsWrites,
    [switch]$AcknowledgePtraceStallAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\startline-tick-recorder-v1\a9tas_hwbp_startline_tick_recorder_v1"
$wrapper = Join-Path $root "src\hwbp_startline_tick_recorder_v1.cpp"
$sharedRecorder = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$protocol = Join-Path $root "src\startline_prearm_protocol_v1.cpp"
$protocolHeader = Join-Path $root "src\startline_prearm_protocol_v1.h"
$validator = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$pins = @{
    $binary = "e126588120ea95dcb87d694315f923884154cd38955f056f52fb31edb145a665"
    $wrapper = "f7d71b882ccaf8d58e7c679bbcfdc537bca4ecedb62ec96afcd789b70677e3c4"
    $sharedRecorder = "5ee6faa7879883a129b0a45cc8fb9994e8b4f815531c80bd7209c82414cd9683"
    $protocol = "16345d7939a8b1e3ff8376828efe4bf2455375ee0fe28bf0179d821fccc56ac7"
    $protocolHeader = "468eb0f40aba1b777334bb5bea96b9ac400866efaaec57f995470bff60189b22"
    $validator = "85cffee892202948a3064f4d89845dac96267545d9769dd39a36c100d5b4b251"
}
$ack = "I_ACCEPT_STARTLINE_PREARM_FIXED_DELTA_BRAKE_CAPTURE_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_startline_tick_recorder_v1"

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments) | Out-String).Trim()
}

if ($TimeoutMs -lt 10000 -or $TimeoutMs -gt 180000) { throw "TimeoutMs must be 10000..180000" }
if ($TargetFrames -lt 60 -or $TargetFrames -gt 3600) { throw "TargetFrames must be 60..3600" }
if ($FixedIntervalUs -ne 16667) { throw "This reviewed candidate requires FixedIntervalUs=16667" }
foreach ($item in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf) -or
        (Get-Sha $item) -ne $pins[$item]) {
        throw "Reviewed startline artifact hash mismatch: $item"
    }
}
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_startline_prearm_protocol_v1 test_startline_tick_recorder_policy_v1 test_synchronized_brake_recording_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Startline source offline tests failed" }
if ($OfflineValidateOnly) {
    Write-Host "STARTLINE_TICK_SOURCE_OFFLINE_VALIDATION_OK"
    Write-Host "ready_phase=read_only_no_attach resume_inputs=1 frame0=first_complete_post_attach_cycle"
    Write-Host "target_frames=$TargetFrames delta_writes=$TargetFrames control_writes=0 physics_writes=0"
    Write-Host "binary_sha256=$($pins[$binary])"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneLiveAttempt, "exactly one live attempt"),
    @($AcknowledgeCountdown3PausedFixedEnvironment, "countdown 3 paused on Ancient Ruins with ZL1"),
    @($AcknowledgeReadyNoAttachBeforeResume, "READY_NO_ATTACH before resume"),
    @($AcknowledgeExactlyOneEscapeResumeInput, "exactly one ESC resume input"),
    @($AcknowledgeUpToTargetFrameFixedDeltaWrites, "$TargetFrames fixed-delta writes"),
    @($AcknowledgeReadOnlySteeringBrakePhysicsCapture, "read-only steering/brake/physics capture"),
    @($AcknowledgeNoControlOrPhysicsWrites, "zero control/physics writes"),
    @($AcknowledgePtraceStallAndCrashRisk, "ptrace stall and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }
$deviceLines = (Invoke-AdbText @("devices")) -split "`r?`n"
if (-not ($deviceLines | Select-String -Quiet "^$([regex]::Escape($Device))\s+device$")) {
    throw "ADB device is not connected: $Device"
}
$pidText = Invoke-AdbText @("-s", $Device, "shell", "su -c 'pidof $package'")
if ($pidText -notmatch '^\d+$') { throw "Game PID unavailable or ambiguous: $pidText" }
$gamePid = [int]$pidText
$readyPath = "/data/local/tmp/a9tas_startline_ready_$gamePid"
$readyState = Invoke-AdbText @("-s", $Device, "shell", "su -c 'test -e $readyPath; echo `$?'")
if ($readyState -ne "1") { throw "Stale startline ready marker exists: $readyPath" }
function Get-Tracer {
    return Invoke-AdbText @("-s", $Device, "shell", "su -c 'grep ^TracerPid: /proc/$gamePid/status'")
}
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Game is already traced" }

$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$bases = @(foreach ($line in $maps) {
    if ($line -match 'libAsphalt9\.so' -and
        $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
        [Convert]::ToUInt64($matches[2], 16) -eq 0) {
        [Convert]::ToUInt64($matches[1], 16)
    }
})
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base, found $($bases.Count)" }
$baseHex = $bases[0].ToString("x")

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($RecordingOutputPath -eq "") { $RecordingOutputPath = Join-Path $root "evidence\a9tas_startline_source_${TargetFrames}f_$stamp.a9utk1" }
if ($ReportOutputPath -eq "") { $ReportOutputPath = Join-Path $root "evidence\a9tas_startline_source_${TargetFrames}f_$stamp.a9usr2" }
$RecordingOutputPath = [IO.Path]::GetFullPath($RecordingOutputPath)
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
if ($RecordingOutputPath -eq $ReportOutputPath) { throw "Recording and report paths must differ" }
foreach ($output in @($RecordingOutputPath, $ReportOutputPath)) {
    if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
}
$remoteRecording = "/data/local/tmp/a9tas_startline_source_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_startline_source_${gamePid}_$stamp.a9usr2"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Startline recorder push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'" | Out-Null
$remoteBinaryHash = Invoke-AdbText @("-s", $Device, "shell", "su -c 'sha256sum $remoteBinary'")
if ($remoteBinaryHash -notmatch '^([0-9a-fA-F]{64})\s+' -or
    $matches[1].ToLower() -ne $pins[$binary]) { throw "Remote recorder hash mismatch" }

$remoteCommand = "$remoteBinary $gamePid $baseHex $TimeoutMs $TargetFrames $FixedIntervalUs $remoteRecording $remoteReport $ack 0 0 0"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in @("-s", $Device, "shell", "su -c '$remoteCommand'")) {
    $startInfo.ArgumentList.Add($argument)
}
$candidate = [Diagnostics.Process]::new()
$candidate.StartInfo = $startInfo
if (-not $candidate.Start()) { throw "Failed to launch resident startline recorder" }

$ready = $false
$readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
while ([DateTime]::UtcNow -lt $readyDeadline -and -not $candidate.HasExited) {
    $state = Invoke-AdbText @("-s", $Device, "shell", "su -c 'test -e $readyPath; echo `$?'")
    if ($state -eq "0") { $ready = $true; break }
    Start-Sleep -Milliseconds 100
}
if (-not $ready) {
    $candidate.WaitForExit()
    throw "Candidate never published READY_NO_ATTACH: $($candidate.StandardError.ReadToEnd())"
}
$readyPayload = Invoke-AdbText @("-s", $Device, "shell", "su -c 'cat $readyPath'")
if ($readyPayload -notmatch '^READY_NO_ATTACH_V1 ' -or
    $readyPayload -notmatch 'target_threads_attached=0 game_writes=0' -or
    $readyPayload -notmatch 'host_resume_gate=marker_removal') {
    throw "Invalid READY_NO_ATTACH marker: $readyPayload"
}
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) {
    throw "Candidate attached before ESC; aborting without resume"
}
Write-Host "STARTLINE_HOST_READY pid=$gamePid marker_verified=1 TracerPid=0 sending_single_ESC=1"
& $AdbPath -s $Device shell input keyevent 111 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "The single ESC resume input failed" }
& $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Host resume acknowledgement failed" }

if (-not $candidate.WaitForExit($TimeoutMs + 15000)) {
    throw "Startline recorder exceeded its bounded timeout; no retry was attempted"
}
$candidateOutput = $candidate.StandardOutput.ReadToEnd()
$candidateError = $candidate.StandardError.ReadToEnd()
if ($candidateOutput) { Write-Host $candidateOutput.TrimEnd() }
if ($candidateError) { Write-Warning $candidateError.TrimEnd() }
$runCode = $candidate.ExitCode

$pidAfter = Invoke-AdbText @("-s", $Device, "shell", "su -c 'pidof $package'")
if ($pidAfter -ne $pidText) { throw "Game process exited or changed PID" }
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Recorder left a tracer attached" }
foreach ($pair in @(@($remoteRecording, $RecordingOutputPath), @($remoteReport, $ReportOutputPath))) {
    $remoteHash = Invoke-AdbText @("-s", $Device, "shell", "su -c 'sha256sum $($pair[0])'")
    if ($remoteHash -notmatch '^([0-9a-fA-F]{64})\s+') { throw "Missing remote artifact: $($pair[0])" }
    $expectedHash = $matches[1].ToLower()
    & $AdbPath -s $Device shell "su -c 'chmod 644 $($pair[0])'" | Out-Null
    & $AdbPath -s $Device pull $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $pair[1]) -ne $expectedHash) { throw "Pulled artifact mismatch: $($pair[1])" }
}
if ($runCode -ne 0) { throw "Startline recorder failed closed (exit=$runCode); no retry was attempted" }
python -B $validator $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "Startline A9USR2/A9UTK1 validation failed" }
Write-Host "STARTLINE_TICK_SOURCE_LIVE_PASSED frames=$TargetFrames single_ESC=1 TracerPid=0"
Write-Host "Recording: $RecordingOutputPath"
Write-Host "Report: $ReportOutputPath"
