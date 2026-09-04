# Guarded synchronized source capture bound to the authoritative race 2 -> 3 edge.
# Default mode is offline-only and never contacts ADB.

param(
    [ValidateSet("OfflineValidate", "ExecuteCapture")]
    [string]$Mode = "OfflineValidate",
    [ValidateSet("Neutral", "SteerDrift", "NaturalActions", "NaturalActionsBarrel")]
    [string]$CaptureProfile = "Neutral",
    [ValidateSet(360, 900)][int]$TargetFrames = 360,
    [ValidateRange(30000, 180000)][int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ObjectHex = "",
    [string]$StateAddressHex = "",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [switch]$ExecuteExactlyOneAttempt,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeCurrentReadOnlyLifecycleAddresses,
    [switch]$AcknowledgeSingleEscBeforeRelease,
    [switch]$Acknowledge360FixedDeltaWrites,
    [switch]$Acknowledge900FixedDeltaWrites,
    [switch]$AcknowledgeReadOnlyControlAndPhysicsCapture,
    [switch]$AcknowledgePassiveNitroVptrHook,
    [switch]$AcknowledgeNoManualInput,
    [switch]$AcknowledgeManualSteerDriftSequenceAfterResume,
    [switch]$AcknowledgeManualNaturalActionSequenceAfterResume,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$barrelCapture = $CaptureProfile -eq "NaturalActionsBarrel"
$naturalCapture = $CaptureProfile -in @("NaturalActions", "NaturalActionsBarrel")
$binary = if ($barrelCapture) {
    Join-Path $root "build\lifecycle-natural-action-barrel-source-v1\a9tas_lifecycle_natural_action_barrel_source_v1_review_only"
} elseif ($naturalCapture) {
    Join-Path $root "build\lifecycle-natural-action-source-v1\a9tas_lifecycle_natural_action_source_v1_review_only"
} else {
    Join-Path $root "build\lifecycle-source-v1\a9tas_lifecycle_source_v1_review_only"
}
$buildScript = if ($barrelCapture) {
    Join-Path $root "build-lifecycle-natural-action-barrel-source-v1.ps1"
} elseif ($naturalCapture) {
    Join-Path $root "build-lifecycle-natural-action-source-v1.ps1"
} else {
    Join-Path $root "build-lifecycle-source-v1.ps1"
}
$source = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$binaryPolicy = if ($naturalCapture) {
    Join-Path $root "tools\test_lifecycle_natural_action_source_policy_v1.py"
} else {
    Join-Path $root "tools\test_lifecycle_source_policy_v1.py"
}
$barrelBinaryPolicy = Join-Path $root "tools\test_lifecycle_barrel_source_policy_v1.py"
$validator = if ($barrelCapture) {
    Join-Path $root "tools\lifecycle_natural_action_barrel_recording_v1.py"
} elseif ($naturalCapture) {
    Join-Path $root "tools\lifecycle_natural_action_recording_v1.py"
} else {
    Join-Path $root "tools\lifecycle_source_recording_v1.py"
}
$actionValidator = Join-Path $root "tools\lifecycle_steer_drift_recording_v1.py"
$physicalValidator = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$preparedReceipt = Join-Path $root "build\natural-action-recording-runner-v1\prepared-process-v1.json"
$recordingPayload = Join-Path $root "build\natural-action-recording-payload-v1\liba9tas_natural_action_recording_v1_review_only.so"
$recordingBootstrap = Join-Path $root "build\natural-action-recording-live-v1\liba9tas_bootstrap_natural_action_recording_v1.so"
$remoteRecordingPayload = "/data/local/tmp/liba9tas_natural_action_recording_v1_review_only.so"
$remoteRecordingBootstrap = "/data/local/tmp/liba9tas_bootstrap_nal_v1.so"
$ack = if ($naturalCapture) { "I_ACCEPT_SYNC_RECORDER_NATURAL_ACTION_V1" } else { "I_ACCEPT_SYNC_RECORDER_RACE_LIFECYCLE_V1" }
$remoteBinary = if ($barrelCapture) {
    "/data/local/tmp/a9tas_lifecycle_natural_action_barrel_source_v1"
} elseif ($naturalCapture) {
    "/data/local/tmp/a9tas_lifecycle_natural_action_source_v1"
} else {
    "/data/local/tmp/a9tas_lifecycle_source_v1"
}
$pins = if ($barrelCapture) {
    @{
        $binary = "a2e71696fdc9fd958df58b4e68dbe91e0980ef9be4efc058c693867aadd3e349"
        $buildScript = "0a5029a79c5efb8d3d675f53a595b29206523a7cabc377eda160d1e3a26b637d"
        $source = "f72f62317e82ae1e3d43ea56c9cf8f9d236dafc404d7a079171616c970f3f66c"
        $binaryPolicy = "5c2b0099bc2405e433e84fd1e64b5b01a59dd4889de4bee3e12ad6c0e3b9a026"
        $barrelBinaryPolicy = "b8eb535c077a1aaa771faea559bf3e01b74a43b4d5e71ead936659d43176cb94"
        $validator = "e4dbe2bd5261f9abf2e47bc8c0154a62acd776eb650441c269a04109fe4678cc"
        $actionValidator = "835c9e0ab3a00c00d5053456083e38b4e262edaaa37b60f30d1cd0c97836218a"
        $physicalValidator = "01a66fe65543d0a5882920aa3dde94251e75057ce1e11da5e5abea189f09fc22"
    }
} elseif ($naturalCapture) {
    @{
        $binary = "c524cbdbbd38d9588b1cd0364e8fb5a273e9d42ea9b3b9e475d1d71792b49c09"
        $buildScript = "1867a7bef380c81872abf3ea21ae0dab5e046d3e75ee9fd66cf20631bcd44dc2"
        $source = "f72f62317e82ae1e3d43ea56c9cf8f9d236dafc404d7a079171616c970f3f66c"
        $binaryPolicy = "5c2b0099bc2405e433e84fd1e64b5b01a59dd4889de4bee3e12ad6c0e3b9a026"
        $validator = "09620f2043ad6a4971d59f258267f29a292884c2955393e34842685899839366"
        $actionValidator = "835c9e0ab3a00c00d5053456083e38b4e262edaaa37b60f30d1cd0c97836218a"
        $physicalValidator = "01a66fe65543d0a5882920aa3dde94251e75057ce1e11da5e5abea189f09fc22"
    }
} else {
    @{
        $binary = "fc226bc166a80b62e5c5b56d7126cf730ffedc671aed7c8d8f6a2f5be7cc0820"
        $buildScript = "a588f9eb053b07a7df2ffefd70d249846658e53fc34db5531e195e39b0b77cea"
        $source = "f72f62317e82ae1e3d43ea56c9cf8f9d236dafc404d7a079171616c970f3f66c"
        $binaryPolicy = "c4d967489748b99c4273d7129dd9d60a830d4b680c99ded4f1b89f5fe2c7a3b7"
        $validator = "0185f9ceca7b720bdc6d3a9caab20a47c4dba76d272d8b59eaa8b2d0e0937b55"
        $actionValidator = "835c9e0ab3a00c00d5053456083e38b4e262edaaa37b60f30d1cd0c97836218a"
        $physicalValidator = "01a66fe65543d0a5882920aa3dde94251e75057ce1e11da5e5abea189f09fc22"
    }
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
function Force-StopPreparedGame {
    & $AdbPath -s $Device shell "am force-stop $package" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Game force-stop failed" }
    for ($poll = 0; $poll -lt 30; ++$poll) {
        Start-Sleep -Milliseconds 100
        if ((Get-GamePid) -eq 0) { return }
    }
    throw "Game remained alive after exact package force-stop"
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

foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing reviewed lifecycle source input: $path"
    }
    if ((Get-Sha $path) -ne $pins[$path]) {
        throw "Lifecycle source pin mismatch: $path"
    }
}
if ($naturalCapture) {
    python -B $binaryPolicy $source $binary
} else {
    python -B $binaryPolicy $binary
}
if ($LASTEXITCODE -ne 0) { throw "Lifecycle source binary policy failed" }
if ($barrelCapture) {
    python -B $barrelBinaryPolicy $source $binary
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle barrel source binary policy failed" }
}

if ($Mode -eq "OfflineValidate") {
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $binary) -ne $pins[$binary]) {
        throw "Lifecycle source build/hash validation failed"
    }
    Write-Output "LIFECYCLE_SOURCE_RUNNER_OFFLINE passed=1 frames=$TargetFrames profile=$CaptureProfile deployed=0 device_access=0"
    return
}

$fixedWriteAcknowledged = if ($TargetFrames -eq 360) {
    $Acknowledge360FixedDeltaWrites
} else {
    $Acknowledge900FixedDeltaWrites
}
foreach ($gate in @(
    @($ExecuteExactlyOneAttempt, "exactly one attempt"),
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, "Ancient Ruins + ZL1 countdown-3 paused"),
    @($AcknowledgeCurrentReadOnlyLifecycleAddresses, "current read-only lifecycle object and state addresses"),
    @($AcknowledgeSingleEscBeforeRelease, "one ESC while all target threads remain frozen"),
    @($fixedWriteAcknowledged, "exactly $TargetFrames fixed-delta writes"),
    @($AcknowledgeReadOnlyControlAndPhysicsCapture, "read-only control and physics capture"),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, "ptrace stall, bounded rollback and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }

if ($CaptureProfile -eq "Neutral") {
    if (-not $AcknowledgeNoManualInput) {
        throw "Neutral capture requires acknowledging no manual input after host resume"
    }
    if ($AcknowledgeManualSteerDriftSequenceAfterResume) {
        throw "Neutral capture cannot acknowledge a manual steer/drift sequence"
    }
} elseif ($CaptureProfile -eq "SteerDrift") {
    if (-not $AcknowledgeManualSteerDriftSequenceAfterResume) {
        throw "SteerDrift capture requires a manual steer + S drift pulse + release sequence after resume"
    }
    if ($AcknowledgeNoManualInput) {
        throw "SteerDrift capture cannot acknowledge no manual input"
    }
} else {
    if ($TargetFrames -ne 900) {
        throw "NaturalActions capture requires the 900-frame synchronized window"
    }
    if (-not $AcknowledgeManualNaturalActionSequenceAfterResume -or
        -not $AcknowledgePassiveNitroVptrHook) {
        throw "NaturalActions capture requires the manual action sequence and passive Nitro vptr hook acknowledgements"
    }
    if ($AcknowledgeNoManualInput -or $AcknowledgeManualSteerDriftSequenceAfterResume) {
        throw "NaturalActions capture acknowledgements conflict"
    }
}

$object = Normalize-HexAddress $ObjectHex "ObjectHex"
$stateAddress = Normalize-HexAddress $StateAddressHex "StateAddressHex"
$objectValue = [Convert]::ToUInt64($object, 16)
$stateValue = [Convert]::ToUInt64($stateAddress, 16)
if ($objectValue -gt ([UInt64]::MaxValue - 0x2d8) -or
    $stateValue -ne ($objectValue + 0x2d8)) {
    throw "StateAddressHex must equal ObjectHex + 0x2D8"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}
$gamePid = Get-GamePid
if ($gamePid -le 0) { throw "Game PID unavailable" }
$startTicks = Get-StartTicks $gamePid
if ((Get-TracerPid $gamePid) -ne 0) { throw "Game is already traced" }
if ($naturalCapture) {
    if (-not (Test-Path -LiteralPath $preparedReceipt -PathType Leaf)) {
        throw "Natural-action prepared-process receipt is missing"
    }
    $prepared = Get-Content -Raw -LiteralPath $preparedReceipt | ConvertFrom-Json
    # Only payload/bootstrap are part of the prepared process identity.  The
    # host recorder is hash-pinned and pushed immediately below, so an offline
    # diagnostic rebuild does not require throwing away a valid preload.
    if ($prepared.version -ne 1 -or
        $prepared.semantics -ne "natural_action_recording_source" -or
        $prepared.device -ne $Device -or [int]$prepared.pid -ne $gamePid -or
        [string]$prepared.start_time -ne $startTicks -or
        $prepared.payload_sha256 -ne (Get-Sha $recordingPayload) -or
        $prepared.bootstrap_sha256 -ne (Get-Sha $recordingBootstrap)) {
        throw "Natural-action prepared process identity mismatch"
    }
    $preparedMaps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
    if (-not $preparedMaps.Contains($remoteRecordingPayload) -or
        -not $preparedMaps.Contains($remoteRecordingBootstrap)) {
        throw "Natural-action payload/bootstrap mappings are missing"
    }
}
$baseHex = Get-LibraryBaseHex $gamePid
$readyPath = "/data/local/tmp/a9tas_input_cycle_source_ready_$gamePid"
if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -ne '1') {
    throw "Stale lifecycle source READY marker exists"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($RecordingOutputPath -eq "") {
    $RecordingOutputPath = Join-Path $root "evidence\a9tas_lifecycle_source_${TargetFrames}f_$stamp.a9utk1"
}
if ($ReportOutputPath -eq "") {
    $reportExtension = if ($naturalCapture) { "a9usr6" } else { "a9usr5" }
    $ReportOutputPath = Join-Path $root "evidence\a9tas_lifecycle_source_${TargetFrames}f_$stamp.$reportExtension"
}
$RecordingOutputPath = [IO.Path]::GetFullPath($RecordingOutputPath)
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
foreach ($path in @($RecordingOutputPath, $ReportOutputPath)) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists: $path" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
}
$remoteRecording = "/data/local/tmp/a9tas_lifecycle_source_${gamePid}_$stamp.a9utk1"
$remoteReportExtension = if ($naturalCapture) { "a9usr6" } else { "a9usr5" }
$remoteReport = "/data/local/tmp/a9tas_lifecycle_source_${gamePid}_$stamp.$remoteReportExtension"
Invoke-AdbChecked @('-s', $Device, 'push', $binary, $remoteBinary) `
    "push lifecycle source candidate" | Out-Null
Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteBinary'") `
    "chmod lifecycle source candidate" | Out-Null
Assert-RemoteHash $remoteBinary $pins[$binary]

$remoteCommand = "$remoteBinary $gamePid $baseHex $TimeoutMs $TargetFrames 16667 $remoteRecording $remoteReport $ack $object $stateAddress 0 0 0"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in @('-s', $Device, 'shell', "su -c '$remoteCommand'")) {
    $startInfo.ArgumentList.Add($argument)
}
$candidate = [Diagnostics.Process]::new()
$candidate.StartInfo = $startInfo
if (-not $candidate.Start()) { throw "Failed to launch lifecycle source candidate" }
$stdoutTask = $candidate.StandardOutput.ReadToEndAsync()
$stderrTask = $candidate.StandardError.ReadToEndAsync()

$ready = $false
$escQueued = $false
$escProcess = $null
$releaseCommitted = $false
$forcedTracerTermination = $false
$primaryFailure = ""
try {
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $readyDeadline -and -not $candidate.HasExited) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -eq '0') {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) { throw "Lifecycle source did not publish READY" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
    $escapedState = [regex]::Escape($stateAddress)
    $readyMutationProof = if ($naturalCapture) {
        "gameplay_state_writes=1 service_vptr_swaps=1 payload_staged=1"
    } else {
        "gameplay_state_writes=0"
    }
    $readyPattern = "^READY_ARMED_RACE_LIFECYCLE_SOURCE_V1 pid=$gamePid controller_pid=(\d+) " +
        "state_address=0x$escapedState state=2 target_threads_attached=([1-9][0-9]*) " +
        "all_target_threads_frozen=1 thread_set_stable=1 freeze_passes=([2-4]) " +
        "$readyMutationProof host_resume_gate=marker_removal$"
    $readyMatch = [regex]::Match($readyText, $readyPattern)
    if (-not $readyMatch.Success) { throw "Lifecycle source READY proof mismatch: $readyText" }
    $controllerPid = [int]$readyMatch.Groups[1].Value
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
        (Get-TracerPid $gamePid) -ne $controllerPid) {
        throw "Armed lifecycle source identity mismatch"
    }
    Write-Host "LIFECYCLE_SOURCE_HOST_READY pid=$gamePid state=2 profile=$CaptureProfile sending_single_ESC_while_frozen=1"
    # Start the synchronous Android input command before release, but do not
    # wait while the recipient is frozen.  Release first, then require the
    # single command to complete successfully.
    $escProcess = Start-SingleEscInjection
    $escQueued = $true
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") `
        "release lifecycle source candidate" | Out-Null
    $releaseCommitted = $true
    Complete-SingleEscInjection $escProcess
    Write-Host "LIFECYCLE_SOURCE_HOST_RELEASED pid=$gamePid ESC_before_release=1"
    if (-not $candidate.WaitForExit($TimeoutMs + 15000)) {
        throw "Lifecycle source timed out; no retry"
    }
} catch {
    $primaryFailure = $_.Exception.Message
}

if ($primaryFailure -ne "" -and -not $candidate.HasExited) {
    try {
        & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
        $releaseCommitted = $true
    } catch {}
    [void]$candidate.WaitForExit($TimeoutMs + 5000)
}
if (-not $candidate.HasExited) {
    $liveTracer = 0
    try {
        if ((Get-GamePid) -eq $gamePid) { $liveTracer = Get-TracerPid $gamePid }
    } catch { $liveTracer = 0 }
    if ($liveTracer -gt 0) {
        $forcedTracerTermination = $true
        & $AdbPath -s $Device shell "su -c 'kill -TERM $liveTracer'" | Out-Null
        [void]$candidate.WaitForExit(5000)
    }
}
if (-not $candidate.HasExited) {
    try { $candidate.Kill($true) } catch {}
    [void]$candidate.WaitForExit(5000)
}
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if ($stdout) { Write-Host $stdout.TrimEnd() }
if ($stderr) { Write-Warning $stderr.TrimEnd() }
& $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null

$postPid = Get-GamePid
$postTracer = if ($postPid -eq $gamePid) { Get-TracerPid $gamePid } else { -1 }
if ($primaryFailure -ne "") {
    if ($naturalCapture) {
        Force-StopPreparedGame
        Remove-Item -LiteralPath $preparedReceipt -Force -ErrorAction SilentlyContinue
        throw "$primaryFailure; natural-action prepared process force-stopped; no retry"
    }
    if ($forcedTracerTermination -or $postTracer -ne 0) {
        throw "$primaryFailure; forced or incomplete tracer cleanup; restart game process; no retry"
    }
    throw "$primaryFailure; bounded rollback complete; no retry"
}
if (-not $escQueued -or -not $releaseCommitted) {
    throw "Lifecycle source host ordering proof incomplete"
}
if ($postPid -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks) {
    if ($naturalCapture) {
        Force-StopPreparedGame
        Remove-Item -LiteralPath $preparedReceipt -Force -ErrorAction SilentlyContinue
    }
    throw "Game process changed during lifecycle source capture"
}
if ($postTracer -ne 0) {
    if ($naturalCapture) {
        Force-StopPreparedGame
        Remove-Item -LiteralPath $preparedReceipt -Force -ErrorAction SilentlyContinue
    }
    throw "Lifecycle source left a tracer attached"
}
if ($candidate.ExitCode -ne 0) {
    if ($naturalCapture) {
        Force-StopPreparedGame
        Remove-Item -LiteralPath $preparedReceipt -Force -ErrorAction SilentlyContinue
    }
    throw "Lifecycle source failed closed (exit=$($candidate.ExitCode)); no retry"
}
$startPattern = 'AUTHORITATIVE_RACE_SOURCE_START_V1 state=3 events=1 ' +
    'event_tid=[1-9][0-9]* event_ns=[0-9]+ tick_watchpoints_armed_before_continue=1'
if ($stdout -notmatch $startPattern) {
    if ($naturalCapture) {
        Force-StopPreparedGame
        Remove-Item -LiteralPath $preparedReceipt -Force -ErrorAction SilentlyContinue
    }
    throw "Authoritative lifecycle-to-tick handoff proof missing; no retry"
}
$doneMagic = if ($naturalCapture) { 'A9USR6' } else { 'A9USR5' }
$donePattern = "SYNCHRONIZED_TICK_RECORDER_${doneMagic}_DONE complete=1 " +
    "captured=$TargetFrames/$TargetFrames delta_writes=$TargetFrames read_errors=0 ptrace_errors=0 " +
    'semantic_errors=0 unexpected_stops=0 clean_detach=1'
if ($stdout -notmatch $donePattern) {
    if ($naturalCapture) {
        Force-StopPreparedGame
        Remove-Item -LiteralPath $preparedReceipt -Force -ErrorAction SilentlyContinue
    }
    throw "Lifecycle source completion proof mismatch; no retry"
}
Invoke-AdbChecked @('-s', $Device, 'shell',
    "su -c 'chmod 644 $remoteRecording $remoteReport'") "chmod lifecycle source reports" | Out-Null
foreach ($pair in @(@($remoteRecording, $RecordingOutputPath), @($remoteReport, $ReportOutputPath))) {
    Invoke-AdbChecked @('-s', $Device, 'pull', $pair[0], $pair[1]) "pull lifecycle source artifact" | Out-Null
}
python -B $validator $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "Lifecycle $doneMagic/A9UTK1 validation failed" }
if ($CaptureProfile -eq "SteerDrift") {
    python -B $actionValidator $ReportOutputPath $RecordingOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw "Lifecycle steer/drift action-content validation failed; no retry"
    }
}
if ($naturalCapture) {
    Remove-Item -LiteralPath $preparedReceipt -Force
}
Write-Output "LIFECYCLE_SOURCE_LIVE_PASSED frames=$TargetFrames profile=$CaptureProfile lifecycle_events=1 single_ESC_before_release=1 TracerPid=0"
Write-Output "Recording: $RecordingOutputPath"
Write-Output "Report: $ReportOutputPath"
