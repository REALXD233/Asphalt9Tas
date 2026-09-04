# Guarded 360-frame final-writer replay bound to the authoritative race 2 -> 3 edge.
# Default mode is offline-only and never contacts ADB.  Live mode requires a
# separately prepared fresh process with the reviewed final-writer payload mapped.

param(
    [ValidateSet("OfflineValidate", "ExecuteReplay")]
    [string]$Mode = "OfflineValidate",
    [ValidateSet("Neutral", "SteerDrift")]
    [string]$ReplayProfile = "Neutral",
    [ValidateRange(30000, 180000)][int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ObjectHex = "",
    [string]$StateAddressHex = "",
    [string]$RecordingPath = "",
    [string]$RecordingSha256 = "",
    [string]$SourceReportPath = "",
    [string]$SourceReportSha256 = "",
    [string]$TargetPath = "",
    [string]$TargetSha256 = "",
    [string]$OutputDirectory = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$ExecuteExactlyOneAttempt,
    [switch]$AcknowledgePreparedFreshProcess,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeCurrentReadOnlyLifecycleAddresses,
    [switch]$AcknowledgeLifecycleSourceAndTargetHashes,
    [switch]$AcknowledgeSingleEscBeforeReleaseAndNoManualInput,
    [switch]$Acknowledge360Delta720ControlAndConditionalPhysicsWrites,
    [switch]$AcknowledgeSteerDriftSourceAndExactControlReplay,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$candidate = Join-Path $root "build\lifecycle-final-writer-v1\a9tas_lifecycle_final_writer_v1_review_only"
$reviewObject = Join-Path $root "build\lifecycle-final-writer-v1\lifecycle_final_writer_v1_review_only.o"
$buildScript = Join-Path $root "build-lifecycle-final-writer-v1.ps1"
$wrapper = Join-Path $root "src\hwbp_lifecycle_final_writer_replay_v1.cpp"
$finalWriterSource = Join-Path $root "src\hwbp_final_writer_unified_replay_v1.cpp"
$executorSource = Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$candidatePolicy = Join-Path $root "tools\test_lifecycle_final_writer_policy_v1.py"
$sourceValidator = Join-Path $root "tools\lifecycle_source_recording_v1.py"
$actionSourceValidator = Join-Path $root "tools\lifecycle_steer_drift_recording_v1.py"
$physicalValidator = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$parser = Join-Path $root "tools\parse_unified_executor_report_v8.py"
$pairValidator = Join-Path $root "tools\validate_final_writer_report_pair_v1.py"
$actionReplayValidator = Join-Path $root "tools\validate_action_control_replay_v1.py"
$payload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$bootstrap = Join-Path $root "build\final-writer-live-gate-v1\liba9tas_bootstrap_final_writer_v1.so"
$receipt = Join-Path $root "build\final-writer-live-gate-v1\prepared-process-v1.json"
$ack = "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1"
$remoteCandidate = "/data/local/tmp/a9tas_lifecycle_final_writer_v1"
$remotePayload = "/data/local/tmp/liba9tas_final_writer_replay_v1_build_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_v1.so"
$pins = @{
    $candidate = "865f6dac8b3c85b35b90d2e6b0d1c0e77759fd0b68b2d0406bca586d79ae0962"
    $reviewObject = "da1a90afc595dc1329c607e3a0dd53d59a06086fb906a2fcaf20839c60a2a8f6"
    $buildScript = "41271a7d3b5e6833fd842f5c659350a7100c87da187b5649a279e17608af2d66"
    $wrapper = "7363c142314dff2616c89d9d55278069b254505c49858711eec0849a14c524c4"
    $finalWriterSource = "9569cea7d0b38e155fe7b31f6cdf33d29060df87651bc30ed2f2aa3b29e17abd"
    $executorSource = "1fef0941bdddfc6478015cdb979e52276dc6ab118c386924dbdcb1eff994eb8b"
    $candidatePolicy = "85da2ed0591367ca59196a4756d078dfaa6ac7b8f4cddb9f284de5532a551ee4"
    $sourceValidator = "0185f9ceca7b720bdc6d3a9caab20a47c4dba76d272d8b59eaa8b2d0e0937b55"
    $actionSourceValidator = "835c9e0ab3a00c00d5053456083e38b4e262edaaa37b60f30d1cd0c97836218a"
    $physicalValidator = "01a66fe65543d0a5882920aa3dde94251e75057ce1e11da5e5abea189f09fc22"
    $parser = "cd929bf2f30c3b82a886a27645d19d6068a53b1e42c9c77c9c8f96e663b71622"
    $pairValidator = "c073a373833451c93effba166de57df82b867a77c3342adcebf109e0fdc77193"
    $actionReplayValidator = "5e59fda8ea174afc19b31272816c337b04f058497489c8f7f61415aa3adaaf02"
    $payload = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
    $bootstrap = "14ab98be3f8cbd6f2a4d15ec98b957779059ca2c3f3119dde9467982053ad82a"
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments 2>&1) | Out-String).Trim()
}
function Invoke-AdbChecked([string[]]$Arguments, [string]$Label) {
    $result = & $AdbPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($result -join ' ')" }
    return $result
}
function Start-SingleEscInjection {
    $process = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'input', 'keyevent', '111') `
        -WindowStyle Hidden -PassThru
    if ($null -eq $process) { throw "Failed to start single ESC injection" }
    return $process
}
function Complete-SingleEscInjection([Diagnostics.Process]$Process) {
    if (-not $Process.WaitForExit(10000)) {
        try { Stop-Process -Id $Process.Id -Force } catch {}
        throw "Single ESC injection timed out after target release"
    }
    if ($Process.ExitCode -ne 0) {
        throw "Single ESC injection failed after target release (exit=$($Process.ExitCode))"
    }
}
function Get-GamePid {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'pidof $package'")
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Get-StartTicks([int]$GamePid) {
    $stat = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed /proc/$GamePid/stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start-time field unavailable"
    }
    return [string]$fields[19]
}
function Get-TracerPid([int]$GamePid) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($line -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid: $line" }
    return [int]$matches[1]
}
function Get-LibraryBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $candidates = @(foreach ($line in $maps) {
        if ($line -match 'libAsphalt9\.so' -and
            $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) {
            $matches[1].ToLowerInvariant()
        }
    })
    $bases = @($candidates | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base" }
    return [string]$bases[0]
}
function Assert-RemoteHash([string]$Remote, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Remote'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Remote"
    }
}
function Normalize-HexAddress([string]$Value, [string]$Label) {
    $normalized = $Value.Trim()
    if ($normalized.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) {
        $normalized = $normalized.Substring(2)
    }
    if ($normalized -notmatch '^[0-9a-fA-F]{8,16}$') {
        throw "$Label must be an 8..16 digit hexadecimal address"
    }
    return $normalized.ToLowerInvariant()
}
function Normalize-Sha256([string]$Value, [string]$Label) {
    $normalized = $Value.Trim().ToLowerInvariant()
    if ($normalized -notmatch '^[0-9a-f]{64}$') { throw "$Label must be a SHA-256" }
    return $normalized
}

foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing reviewed lifecycle final-writer input: $path"
    }
    if ((Get-Sha $path) -ne $pins[$path]) {
        throw "Lifecycle final-writer pin mismatch: $path"
    }
}
python -B $candidatePolicy $reviewObject
if ($LASTEXITCODE -ne 0) { throw "Lifecycle final-writer candidate policy failed" }
if ($Mode -eq "OfflineValidate") {
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $candidate) -ne $pins[$candidate] -or
        (Get-Sha $reviewObject) -ne $pins[$reviewObject]) {
        throw "Lifecycle final-writer build/hash validation failed"
    }
    Write-Output "LIFECYCLE_FINAL_WRITER_RUNNER_OFFLINE passed=1 frames=360 deployed=0 device_access=0"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneAttempt, "exactly one attempt"),
    @($AcknowledgePreparedFreshProcess, "separately prepared fresh process and mapped payload"),
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, "Ancient Ruins + ZL1 countdown-3 paused"),
    @($AcknowledgeCurrentReadOnlyLifecycleAddresses, "current read-only lifecycle object and state addresses"),
    @($AcknowledgeLifecycleSourceAndTargetHashes, "lifecycle source report, recording and target hashes"),
    @($AcknowledgeSingleEscBeforeReleaseAndNoManualInput, "one ESC before release and no manual input"),
    @($Acknowledge360Delta720ControlAndConditionalPhysicsWrites, "360 delta, 720 control-pair and conditional physics writes"),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, "ptrace stall, bounded rollback and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }

if ($ReplayProfile -eq "SteerDrift") {
    if (-not $AcknowledgeSteerDriftSourceAndExactControlReplay) {
        throw "SteerDrift replay requires acknowledging the action source and exact C98/C9C control proof"
    }
} elseif ($AcknowledgeSteerDriftSourceAndExactControlReplay) {
    throw "Neutral replay cannot acknowledge a SteerDrift action source"
}

$object = Normalize-HexAddress $ObjectHex "ObjectHex"
$stateAddress = Normalize-HexAddress $StateAddressHex "StateAddressHex"
$objectValue = [Convert]::ToUInt64($object, 16)
$stateValue = [Convert]::ToUInt64($stateAddress, 16)
if ($objectValue -gt ([UInt64]::MaxValue - 0x2d8) -or
    $stateValue -ne ($objectValue + 0x2d8)) {
    throw "StateAddressHex must equal ObjectHex + 0x2D8"
}
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid optional address: $value" }
}
if ($RecordingPath -eq "" -or $SourceReportPath -eq "" -or $TargetPath -eq "") {
    throw "ExecuteReplay requires RecordingPath, SourceReportPath and TargetPath"
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$SourceReportPath = [IO.Path]::GetFullPath($SourceReportPath)
$TargetPath = [IO.Path]::GetFullPath($TargetPath)
foreach ($path in @($RecordingPath, $SourceReportPath, $TargetPath, $receipt)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing live input: $path" }
}
$expectedRecordingHash = Normalize-Sha256 $RecordingSha256 "RecordingSha256"
$expectedSourceReportHash = Normalize-Sha256 $SourceReportSha256 "SourceReportSha256"
$expectedTargetHash = Normalize-Sha256 $TargetSha256 "TargetSha256"
if ((Get-Sha $RecordingPath) -ne $expectedRecordingHash -or
    (Get-Sha $SourceReportPath) -ne $expectedSourceReportHash -or
    (Get-Sha $TargetPath) -ne $expectedTargetHash) {
    throw "Lifecycle source or target hash mismatch"
}
python -B $sourceValidator $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) { throw "A9USR5/A9UTK1 source validation failed" }
if ($ReplayProfile -eq "SteerDrift") {
    python -B $actionSourceValidator $SourceReportPath $RecordingPath
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle SteerDrift source validation failed; no device opened" }
}
$recordingBytes = [IO.File]::ReadAllBytes($RecordingPath)
$targetBytes = [IO.File]::ReadAllBytes($TargetPath)
if ($recordingBytes.Length -ne 96 + 360 * 144 -or
    [BitConverter]::ToUInt32($recordingBytes, 20) -ne 360 -or
    [BitConverter]::ToUInt32($recordingBytes, 24) -ne 16667) {
    throw "Lifecycle A9UTK1 recording shape mismatch"
}
if ($targetBytes.Length -ne 128 + 360 * 80 -or
    [BitConverter]::ToUInt32($targetBytes, 20) -ne 360) {
    throw "Lifecycle A9FWT1 target shape mismatch"
}
$recordingDigest = [Convert]::FromHexString($expectedRecordingHash)
for ($index = 0; $index -lt 32; ++$index) {
    if ($targetBytes[40 + $index] -ne $recordingDigest[$index]) {
        throw "A9FWT1 is not SHA-bound to the lifecycle A9UTK1"
    }
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}
$prepared = Get-Content -Raw $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
if ($gamePid -le 0 -or [int]$prepared.pid -ne $gamePid -or
    [string]$prepared.device -ne $Device -or
    [string]$prepared.start_ticks -ne (Get-StartTicks $gamePid) -or
    [string]$prepared.payload_sha256 -ne $pins[$payload] -or
    [string]$prepared.bootstrap_sha256 -ne $pins[$bootstrap]) {
    throw "Prepared process identity mismatch"
}
$startTicks = [string]$prepared.start_ticks
if ((Get-TracerPid $gamePid) -ne 0) { throw "Game is already traced" }
$maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
    throw "Prepared final-writer payload/bootstrap are not mapped"
}
$baseHex = Get-LibraryBaseHex $gamePid
$readyPath = "/data/local/tmp/a9tas_final_writer_ready_$gamePid"
if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -ne '1') {
    throw "Stale lifecycle final-writer READY marker exists"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($OutputDirectory -eq "") { $OutputDirectory = Join-Path $root "evidence" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$localUnified = Join-Path $OutputDirectory "a9tas_lifecycle_final_writer_360f_$stamp.a9uer8"
$localPayloadReport = Join-Path $OutputDirectory "a9tas_lifecycle_final_writer_360f_$stamp.a9fwr1"
foreach ($path in @($localUnified, $localPayloadReport)) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists: $path" }
}
$remoteRecording = "/data/local/tmp/a9tas_lifecycle_final_writer_${gamePid}_$stamp.a9utk1"
$remoteTarget = "/data/local/tmp/a9tas_lifecycle_final_writer_${gamePid}_$stamp.a9fwt1"
$remoteUnified = "/data/local/tmp/a9tas_lifecycle_final_writer_${gamePid}_$stamp.a9uer8"
$remotePayloadReport = "/data/local/tmp/a9tas_lifecycle_final_writer_${gamePid}_$stamp.a9fwr1"
foreach ($pair in @(@($candidate, $remoteCandidate, $pins[$candidate]),
                    @($RecordingPath, $remoteRecording, $expectedRecordingHash),
                    @($TargetPath, $remoteTarget, $expectedTargetHash))) {
    Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) "push lifecycle replay artifact" | Out-Null
    Assert-RemoteHash $pair[1] $pair[2]
}
Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteCandidate'") `
    "chmod lifecycle final-writer candidate" | Out-Null

$remoteCommand = "$remoteCandidate $gamePid $baseHex $TimeoutMs $startTicks $remoteRecording $remoteTarget $remoteUnified $remotePayloadReport $ack $object $stateAddress $PhysicsContextHex $MainObjectHex $FinalOwnerHex"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in @('-s', $Device, 'shell', "su -c '$remoteCommand'")) {
    $startInfo.ArgumentList.Add($argument)
}
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) { throw "Failed to launch lifecycle final-writer candidate" }
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()

$escQueued = $false
$escProcess = $null
$releaseCommitted = $false
$forcedTracerTermination = $false
$primaryFailure = ""
try {
    $ready = $false
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $readyDeadline -and -not $process.HasExited) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -eq '0') {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) { throw "Lifecycle final-writer did not publish READY" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
    $escapedState = [regex]::Escape($stateAddress)
    $readyPattern = "^READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1 pid=$gamePid start_ticks=$startTicks " +
        "controller_pid=(\d+) delta=0x[0-9a-f]+ state_address=0x$escapedState state=2 " +
        "target_threads_attached=[1-9][0-9]* vptr_writes=1 gameplay_state_writes=0 " +
        "payload_mapped=1 all_target_threads_frozen=1 host_resume_gate=marker_removal$"
    $readyMatch = [regex]::Match($readyText, $readyPattern)
    if (-not $readyMatch.Success) { throw "Lifecycle final-writer READY proof mismatch: $readyText" }
    $controllerPid = [int]$readyMatch.Groups[1].Value
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
        (Get-TracerPid $gamePid) -ne $controllerPid) {
        throw "Armed lifecycle final-writer identity mismatch"
    }
    Write-Host "LIFECYCLE_FINAL_WRITER_HOST_READY pid=$gamePid state=2 sending_single_ESC_while_frozen=1"
    # Start input before release, then unblock the target before waiting for
    # Android's synchronous input command to receive its acknowledgement.
    $escProcess = Start-SingleEscInjection
    $escQueued = $true
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") `
        "release lifecycle final-writer candidate" | Out-Null
    $releaseCommitted = $true
    Complete-SingleEscInjection $escProcess
    Write-Host "LIFECYCLE_FINAL_WRITER_HOST_RELEASED pid=$gamePid ESC_before_release=1"
    if (-not $process.WaitForExit($TimeoutMs + 15000)) {
        throw "Lifecycle final-writer timed out; no retry"
    }
} catch {
    $primaryFailure = $_.Exception.Message
}

if ($primaryFailure -ne "" -and -not $process.HasExited) {
    try {
        & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
        $releaseCommitted = $true
    } catch {}
    [void]$process.WaitForExit($TimeoutMs + 5000)
}
if (-not $process.HasExited) {
    $liveTracer = 0
    try { if ((Get-GamePid) -eq $gamePid) { $liveTracer = Get-TracerPid $gamePid } } catch { $liveTracer = 0 }
    if ($liveTracer -gt 0) {
        $forcedTracerTermination = $true
        & $AdbPath -s $Device shell "su -c 'kill -TERM $liveTracer'" | Out-Null
        [void]$process.WaitForExit(5000)
    }
}
if (-not $process.HasExited) {
    try { $process.Kill($true) } catch {}
    [void]$process.WaitForExit(5000)
}
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if ($stdout) { Write-Host $stdout.TrimEnd() }
if ($stderr) { Write-Warning $stderr.TrimEnd() }
& $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
$postPid = Get-GamePid
$postTracer = if ($postPid -eq $gamePid) { Get-TracerPid $gamePid } else { -1 }
if ($primaryFailure -ne "") {
    if ($forcedTracerTermination -or $postTracer -ne 0) {
        throw "$primaryFailure; forced or incomplete tracer cleanup; restart game process; no retry"
    }
    throw "$primaryFailure; bounded rollback complete; no retry"
}
if (-not $escQueued -or -not $releaseCommitted) {
    throw "Lifecycle final-writer host ordering proof incomplete"
}
if ($postPid -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks) {
    throw "Game process changed during lifecycle final-writer replay"
}
if ($postTracer -ne 0) { throw "Lifecycle final-writer left a tracer attached" }
if ($process.ExitCode -ne 0) {
    throw "Lifecycle final-writer failed closed (exit=$($process.ExitCode)); no retry"
}
$startPattern = 'AUTHORITATIVE_RACE_START_V1 state=3 events=1 ' +
    'event_tid=[1-9][0-9]* event_ns=[0-9]+ tick_watchpoints_armed_before_continue=1'
if ($stdout -notmatch $startPattern) {
    throw "Authoritative lifecycle-to-replay handoff proof missing; no retry"
}
Invoke-AdbChecked @('-s', $Device, 'shell',
    "su -c 'chmod 644 $remoteUnified $remotePayloadReport'") "chmod lifecycle replay reports" | Out-Null
foreach ($pair in @(@($remoteUnified, $localUnified), @($remotePayloadReport, $localPayloadReport))) {
    Invoke-AdbChecked @('-s', $Device, 'pull', $pair[0], $pair[1]) "pull lifecycle replay report" | Out-Null
}
python -B $parser $localUnified
if ($LASTEXITCODE -ne 0) { throw "Lifecycle A9UER8 validation failed" }
python -B $pairValidator $localUnified $localPayloadReport $TargetPath $payload
if ($LASTEXITCODE -ne 0) { throw "Lifecycle A9UER8/A9FWR1 cross-validation failed" }
if ($ReplayProfile -eq "SteerDrift") {
    python -B $actionReplayValidator $RecordingPath $localUnified
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle SteerDrift exact control replay validation failed; no retry" }
}
$hostWitness = Join-Path $OutputDirectory "a9tas_lifecycle_final_writer_360f_$stamp.host.txt"
$witnessLines = @(
    "LIFECYCLE_FINAL_WRITER_HOST_WITNESS_V1",
    "pid=$gamePid",
    "start_ticks=$startTicks",
    "object=0x$object",
    "state_address=0x$stateAddress",
    "recording_sha256=$expectedRecordingHash",
    "source_report_sha256=$expectedSourceReportHash",
    "target_sha256=$expectedTargetHash",
    "single_ESC_before_release=1",
    "TracerPid=0",
    "--- native stdout ---",
    $stdout.Trim()
)
[IO.File]::WriteAllText(
    $hostWitness,
    ($witnessLines -join [Environment]::NewLine) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
$hostWitnessHash = Get-Sha $hostWitness
Write-Output "LIFECYCLE_FINAL_WRITER_LIVE_PASSED frames=360 profile=$ReplayProfile lifecycle_events=1 single_ESC_before_release=1 TracerPid=0"
Write-Output "Unified report: $localUnified"
Write-Output "Payload report: $localPayloadReport"
Write-Output "Host witness: $hostWitness sha256=$hostWitnessHash"
