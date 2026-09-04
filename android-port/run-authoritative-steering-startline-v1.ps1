# GUARDED start-line authoritative steering replay. This file is not authorization.
# No physical anchor is loaded or searched. The resident candidate publishes
# READY_NO_ATTACH while countdown 3 is paused; the host verifies TracerPid=0,
# sends one ESC, and frame 0 is selected on the first complete post-attach tick.

param(
    [Parameter(Mandatory=$true)][string]$RecordingPath,
    [Parameter(Mandatory=$true)][string]$RecordingSha256,
    [int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$ReplayRecordedBrake,
    [switch]$OfflineValidateOnly,
    [switch]$ExecuteExactlyOneLiveAttempt,
    [switch]$AcknowledgeStartlineV1SourceAndSameFixedEnvironment,
    [switch]$AcknowledgeReadyNoAttachBeforeResume,
    [switch]$AcknowledgeExactlyOneEscapeResumeInput,
    [switch]$AcknowledgeFrameCountFixedDeltaWrites,
    [switch]$AcknowledgeTwiceFrameCountSteeringPairWrites,
    [switch]$AcknowledgeSteeringOnlyBrakePreserved,
    [switch]$AcknowledgeRecordedBrakePairWrites,
    [switch]$AcknowledgeAllOtherCapabilitiesAbsent,
    [switch]$AcknowledgePtraceStallAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$steeringBinary = Join-Path $root "build\authoritative-steering-startline-v1\a9tas_hwbp_authoritative_steering_startline_v1"
$controlsBinary = Join-Path $root "build\authoritative-controls-startline-v1\a9tas_hwbp_authoritative_controls_startline_v1"
$source = Join-Path $root "src\hwbp_authoritative_steering_v1.cpp"
$controlsSource = Join-Path $root "src\authoritative_controls_transport_v1.cpp"
$controlsHeader = Join-Path $root "src\authoritative_controls_transport_v1.h"
$protocol = Join-Path $root "src\startline_prearm_protocol_v1.cpp"
$protocolHeader = Join-Path $root "src\startline_prearm_protocol_v1.h"
$validator = Join-Path $root "tools\validate_authoritative_steering_v1.py"
$controlsValidator = Join-Path $root "tools\validate_authoritative_controls_startline_v1.py"
$binary = if ($ReplayRecordedBrake) { $controlsBinary } else { $steeringBinary }
$pins = @{
    $steeringBinary = "834dbe1aaf75e5bf51b87f90c7573149de48f1fd2a5abbe86f6cd717d442ab76"
    $controlsBinary = "6ed4511d9333b8e2e063cddf4bfdbbff6f51bdee0c19f8283435be60f33779bf"
    $source = "55a3202a7208a841c9e745def6c0626773a11afa8faaf3d7390b7b3f2ba32733"
    $controlsSource = "ab08c8dd12cebf38b973394580fa008477982230e50f8cc43459dafa2a84ac8e"
    $controlsHeader = "e4c8dabf76e2497b785d573db94ee38998b8f7d643f581b42500fd502a540a8e"
    $protocol = "16345d7939a8b1e3ff8376828efe4bf2455375ee0fe28bf0179d821fccc56ac7"
    $protocolHeader = "468eb0f40aba1b777334bb5bea96b9ac400866efaaec57f995470bff60189b22"
    $validator = "1eae53cbc459da77d8cb7bfad07cece3639e668a1106b093dac184f59335dd4e"
    $controlsValidator = "65bf38ec7acd4871606ecabec3d09b4d996d0e65eaf210e724fa8b13614a933c"
}
$ack = if ($ReplayRecordedBrake) {
    "I_ACCEPT_STARTLINE_DIRECT_DELTA_AND_2X_STEERING_BRAKE_WRITES_V1"
} else {
    "I_ACCEPT_STARTLINE_DIRECT_DELTA_AND_2X_STEERING_WRITES_V1"
}
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = if ($ReplayRecordedBrake) {
    "/data/local/tmp/a9tas_hwbp_authoritative_controls_startline_v1"
} else {
    "/data/local/tmp/a9tas_hwbp_authoritative_steering_startline_v1"
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments) | Out-String).Trim()
}

if ($TimeoutMs -lt 10000 -or $TimeoutMs -gt 180000) { throw "TimeoutMs must be 10000..180000" }
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid address: $value" }
}
foreach ($item in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf) -or
        (Get-Sha $item) -ne $pins[$item]) {
        throw "Reviewed startline replay artifact hash mismatch: $item"
    }
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
if (-not (Test-Path -LiteralPath $RecordingPath -PathType Leaf)) { throw "Recording is missing: $RecordingPath" }
$RecordingSha256 = $RecordingSha256.ToLowerInvariant()
if ($RecordingSha256 -notmatch '^[0-9a-f]{64}$' -or (Get-Sha $RecordingPath) -ne $RecordingSha256) {
    throw "Recording hash mismatch"
}
$recordingBytes = [IO.File]::ReadAllBytes($RecordingPath)
if ($recordingBytes.Length -lt 96) { throw "Recording header is truncated" }
$frameCount = [BitConverter]::ToUInt32($recordingBytes, 20)
if ($frameCount -lt 1 -or $frameCount -gt 36000) { throw "Recording frame count outside 1..36000" }
if ($recordingBytes.Length -ne 96 + $frameCount * 144) { throw "Recording length/frame count mismatch" }
if ([BitConverter]::ToUInt32($recordingBytes, 24) -ne 16667) { throw "Recording fixed interval must be 16667" }
$requiredSkip = if ($ReplayRecordedBrake) { 0xfc } else { 0xfe }
for ($index = 0; $index -lt $frameCount; ++$index) {
    $skip = [BitConverter]::ToUInt32($recordingBytes, 96 + $index * 144 + 32)
    if ($skip -ne $requiredSkip) {
        throw "Recording frame $index has skip mask 0x$($skip.ToString('x')); required=0x$($requiredSkip.ToString('x'))"
    }
}
$pairCount = $frameCount * 2
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_startline_prearm_protocol_v1 test_authoritative_steering_startline_policy_v1 test_authoritative_steering_executor_v1 test_authoritative_controls_transport_v1 test_authoritative_controls_startline_v1
    $testCode = $LASTEXITCODE
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Startline replay offline tests failed" }
python -B $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "A9AST1 validator selftest failed" }
python -B $controlsValidator --selftest
if ($LASTEXITCODE -ne 0) { throw "A9AST1 controls validator selftest failed" }
if ($OfflineValidateOnly) {
    Write-Host "AUTHORITATIVE_STEERING_STARTLINE_OFFLINE_VALIDATION_OK"
    Write-Host "frames=$frameCount delta_writes=$frameCount pair_writes=$pairCount physical_anchor_search=0"
    Write-Host "ready_phase=read_only_no_attach resume_inputs=1 frame0=first_complete_post_attach_cycle"
    if ($ReplayRecordedBrake) {
        Write-Host "scope=steering+recorded-brake other_capabilities=absent"
    } else {
        Write-Host "scope=steering-only brake=preserved other_capabilities=absent"
    }
    Write-Host "binary_sha256=$($pins[$binary]) recording_sha256=$RecordingSha256"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneLiveAttempt, "exactly one live attempt"),
    @($AcknowledgeStartlineV1SourceAndSameFixedEnvironment, "a startline-v1 source and the same Ancient Ruins/ZL1 environment"),
    @($AcknowledgeReadyNoAttachBeforeResume, "READY_NO_ATTACH before resume"),
    @($AcknowledgeExactlyOneEscapeResumeInput, "exactly one ESC resume input"),
    @($AcknowledgeFrameCountFixedDeltaWrites, "$frameCount fixed-delta writes"),
    @($AcknowledgeTwiceFrameCountSteeringPairWrites, "$pairCount control-pair writes"),
    @($AcknowledgeAllOtherCapabilitiesAbsent, "all other capabilities absent"),
    @($AcknowledgePtraceStallAndCrashRisk, "ptrace stall and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if ($ReplayRecordedBrake) {
    if (-not $AcknowledgeRecordedBrakePairWrites -or
        $AcknowledgeSteeringOnlyBrakePreserved) {
        throw "Recorded-brake mode requires only AcknowledgeRecordedBrakePairWrites"
    }
} elseif (-not $AcknowledgeSteeringOnlyBrakePreserved -or
          $AcknowledgeRecordedBrakePairWrites) {
    throw "Steering-only mode requires only AcknowledgeSteeringOnlyBrakePreserved"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }
if (-not (Invoke-AdbText @("devices") | Select-String -Quiet "^$([regex]::Escape($Device))\s+device$")) {
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
if ($OutputPath -eq "") { $OutputPath = Join-Path $root "evidence\a9tas_authoritative_startline_${frameCount}f_$stamp.a9ast1" }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) { throw "Output already exists: $OutputPath" }
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
$remoteRecording = "/data/local/tmp/a9tas_startline_replay_${frameCount}f_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_startline_replay_${gamePid}_$stamp.a9ast1"

foreach ($pair in @(@($binary, $remoteBinary, $pins[$binary]), @($RecordingPath, $remoteRecording, $RecordingSha256))) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair[0])" }
    $remoteHash = Invoke-AdbText @("-s", $Device, "shell", "su -c 'sha256sum $($pair[1])'")
    if ($remoteHash -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLower() -ne $pair[2]) { throw "Remote hash mismatch: $($pair[1])" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'" | Out-Null
$unusedAnchor = "/data/local/tmp/NO_A9NPA1_USED_STARTLINE_DIRECT"
$remoteCommand = "$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $unusedAnchor $remoteReport $ack $PhysicsContextHex $MainObjectHex $FinalOwnerHex"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in @("-s", $Device, "shell", "su -c '$remoteCommand'")) { $startInfo.ArgumentList.Add($argument) }
$candidate = [Diagnostics.Process]::new()
$candidate.StartInfo = $startInfo
if (-not $candidate.Start()) { throw "Failed to launch resident startline replay" }

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
    $readyPayload -notmatch 'target_threads_attached=0 game_writes=0') { throw "Invalid READY_NO_ATTACH marker" }
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Candidate attached before ESC" }
Write-Host "STARTLINE_REPLAY_HOST_READY pid=$gamePid frames=$frameCount TracerPid=0 sending_single_ESC=1"
& $AdbPath -s $Device shell input keyevent 111 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "The single ESC resume input failed" }

if (-not $candidate.WaitForExit($TimeoutMs + 15000)) { throw "Startline replay exceeded its bounded timeout; no retry was attempted" }
$candidateOutput = $candidate.StandardOutput.ReadToEnd()
$candidateError = $candidate.StandardError.ReadToEnd()
if ($candidateOutput) { Write-Host $candidateOutput.TrimEnd() }
if ($candidateError) { Write-Warning $candidateError.TrimEnd() }
$runCode = $candidate.ExitCode
$pidAfter = Invoke-AdbText @("-s", $Device, "shell", "su -c 'pidof $package'")
if ($pidAfter -ne $pidText) { throw "Game process exited or changed PID" }
if ((Get-Tracer) -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Replay left a tracer attached" }
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Replay report pull failed" }
if ($runCode -ne 0) {
    if (-not $ReplayRecordedBrake) {
        python -B $validator --diagnostic $OutputPath
    }
    throw "Startline replay failed closed (exit=$runCode); no retry was attempted"
}
if ($ReplayRecordedBrake) {
    python -B $controlsValidator $OutputPath $RecordingPath
} else {
    python -B $validator $OutputPath
}
if ($LASTEXITCODE -ne 0) { throw "Strict startline A9AST1 validation failed" }
$scope = if ($ReplayRecordedBrake) { "steering+recorded-brake" } else { "steering-only" }
Write-Host "AUTHORITATIVE_STARTLINE_LIVE_PASSED scope=$scope frames=$frameCount delta=$frameCount pairs=$pairCount single_ESC=1 TracerPid=0"
Write-Host "Report: $OutputPath"
