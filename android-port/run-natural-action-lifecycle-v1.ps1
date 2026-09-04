param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ExecuteCountdownGate")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 5000,
    [switch]$ActionSchedulerReview,
    [switch]$SequenceReplayReview,
    [switch]$RecordingSourceReview,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeRacePausedNitroIdleUsable,
    [switch]$AcknowledgeExactlyOneEscapeResume,
    [switch]$AcknowledgeOneVptrSwapAndPayloadWrites,
    [switch]$AcknowledgeZeroActionAndNitroCalls,
    [switch]$AcknowledgeExactlyOneNaturalNitroActivation,
    [switch]$AcknowledgeFiveFrameNaturalNitroSequence,
    [switch]$AcknowledgeFailureForceStopsFreshProcess,
    [switch]$AcknowledgePtraceStallAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_natural_action_lifecycle_preload_v1.sh"
$selectedReviews = @($ActionSchedulerReview, $SequenceReplayReview, $RecordingSourceReview) |
    Where-Object { $_ }
if ($selectedReviews.Count -gt 1) {
    throw "ActionSchedulerReview, SequenceReplayReview and RecordingSourceReview are mutually exclusive"
}
$semantics = if ($RecordingSourceReview) {
    "natural_action_recording_source"
} elseif ($SequenceReplayReview) {
    "natural_nitro_sequence_0_1_0_2_0"
} elseif ($ActionSchedulerReview) {
    "one_natural_nitro_activation"
} else {
    "zero_call"
}
if ($RecordingSourceReview) {
    $outDir = Join-Path $root "build\natural-action-recording-runner-v1"
    $payload = Join-Path $root "build\natural-action-recording-payload-v1\liba9tas_natural_action_recording_v1_review_only.so"
    $candidate = Join-Path $root "build\lifecycle-natural-action-source-v1\a9tas_lifecycle_natural_action_source_v1_review_only"
    $bootstrap = Join-Path $root "build\natural-action-recording-live-v1\liba9tas_bootstrap_natural_action_recording_v1.so"
    $validator = Join-Path $root "tools\lifecycle_natural_action_recording_v1.py"
    $payloadPin = "ad3ae03543b7489102adb53dba320296e93b034b12fe18bcf216969bc123c527"
    $candidatePin = "c524cbdbbd38d9588b1cd0364e8fb5a273e9d42ea9b3b9e475d1d71792b49c09"
    $bootstrapPin = "44757629fcabc94517a05ec84fb2161d02bdaf424deeda9c84f469186d9b4d97"
    $validatorPin = "09620f2043ad6a4971d59f258267f29a292884c2955393e34842685899839366"
    $remotePayload = "/data/local/tmp/liba9tas_natural_action_recording_v1_review_only.so"
    $ack = "I_ACCEPT_SYNC_RECORDER_NATURAL_ACTION_V1"
} elseif ($SequenceReplayReview) {
    $outDir = Join-Path $root "build\natural-action-replay-sequence-runner-v1"
    $payload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
    $candidate = Join-Path $root "build\natural-action-replay-sequence-live-candidate-v1\natural_action_replay_sequence_candidate_v1"
    $bootstrap = Join-Path $root "build\natural-action-replay-sequence-live-candidate-v1\liba9tas_bootstrap_nar5_v1.so"
    $validator = Join-Path $root "tools\validate_natural_action_replay_sequence_report_v1.py"
    $payloadPin = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
    $candidatePin = "0be4583e0fe336c6e8d73a3cb07690b1913ca04b5ed6736f225a1c62611a555c"
    $bootstrapPin = "85995fa56ef4de5f7f8507179aa50f5fd7891454b29f5dfcddfd5d9dcdef0e5a"
    $validatorPin = "4ce9c94d66954049e8399bc2165339e7b12b454e26a6e24101c0291f1b314d17"
    $remotePayload = "/data/local/tmp/liba9tas_natural_action_replay_v1_review_only.so"
    $ack = "I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1"
} elseif ($ActionSchedulerReview) {
    $outDir = Join-Path $root "build\natural-action-scheduler-runner-v1"
    $payload = Join-Path $root "build\natural-action-scheduler-payload-v1\liba9tas_natural_action_scheduler_v1_review_only.so"
    $candidate = Join-Path $root "build\natural-action-scheduler-live-candidate-v1\natural_action_scheduler_candidate_v1"
    $bootstrap = Join-Path $root "build\natural-action-scheduler-live-candidate-v1\liba9tas_bootstrap_nas_v1.so"
    $validator = Join-Path $root "tools\validate_natural_action_scheduler_report_v1.py"
    $payloadPin = "d718a13578e7e37a3d2a0240f94b125620858d9657d69a4fc799b8c5263b7f9e"
    $candidatePin = "bf2b772dcffd8346279193d56bc196e23b35a029aa7358b5834b62d5c6410fe7"
    $bootstrapPin = "fc8e977465d967e92f5c1b1b3908c340105e5c96a0a5982cfeb7506cfb8f5fb4"
    $validatorPin = "60985bf0bcc02861a4c1e95fb678f0912bb894c214d2aa8fb354401b03dbe9c7"
    $remotePayload = "/data/local/tmp/liba9tas_natural_action_scheduler_v1_review_only.so"
    $ack = "I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1"
} else {
    $outDir = Join-Path $root "build\natural-action-lifecycle-runner-v1"
    $payload = Join-Path $root "build\natural-action-callback-lifecycle-v1\liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
    $candidate = Join-Path $root "build\natural-action-lifecycle-live-candidate-v1\natural_action_lifecycle_candidate_v1"
    $bootstrap = Join-Path $root "build\natural-action-lifecycle-live-candidate-v1\liba9tas_bootstrap_nal_v1.so"
    $validator = Join-Path $root "tools\validate_natural_action_lifecycle_report_v1.py"
    $payloadPin = "60a726174ac2a613d6098db77f9e82bbf819923ffd4e798d1f66eda9db454a41"
    $candidatePin = "8dbabac8ddb46cd1e6229d904b36a5cb3216f8b49191c6312c4053ac909081fd"
    $bootstrapPin = "5629368c8a39e87f3a45ee854eb93642b72622307612f20607675a9242b27760"
    $validatorPin = "70f667804e84bbf66f7b62f4e994ae92c9d36c196e152716a12127d14a4df0fd"
    $remotePayload = "/data/local/tmp/liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
    $ack = "I_ACCEPT_ZERO_CALL_LIFECYCLE_REVIEW_V1"
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$receipt = Join-Path $outDir "prepared-process-v1.json"
$pins = @{
    $payload = $payloadPin
    $candidate = $candidatePin
    $bootstrap = $bootstrapPin
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "8a030d91ce366c48a6f92da3c1cda10ca65cea7fac7dec9e6e122cfc4146c571"
    $validator = $validatorPin
}
$remoteCandidate = "/data/local/tmp/a9tas_nal_candidate_v1"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_nal_v1.so"
$remoteInjector = "/data/local/tmp/a9tas_injector_nal_v1"
$remoteHelper = "/data/local/tmp/run_natural_action_lifecycle_preload_v1.sh"

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    $text = ((& $AdbPath @Arguments 2>&1) -join "`n").Trim()
    if ($LASTEXITCODE -ne 0) { throw "ADB command failed: $text" }
    $text
}
function Get-GamePid {
    $text = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
    if ($text -match '^\d+$') { return [int]$text }
    return 0
}
function Get-StartTime([int]$GamePid) {
    $text = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    if ($text -notmatch '^\d+\s+\(.*\)\s+\S\s+(.+)$') { throw "Process stat parse failed" }
    $fields = @($matches[1] -split '\s+')
    if ($fields.Count -lt 19 -or $fields[18] -notmatch '^\d+$') { throw "Process start time parse failed" }
    $fields[18]
}
function Get-Tracer([int]$GamePid) {
    Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
}
function Assert-CleanTracer([int]$GamePid) {
    $line = Get-Tracer $GamePid
    if ($line -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Active tracer: $line" }
}
function Assert-StableCleanTracer([int]$GamePid) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds(2000)
    $consecutive = 0
    $last = "unread"
    while ([DateTime]::UtcNow -lt $deadline) {
        $last = Get-Tracer $GamePid
        if ($last -in @("TracerPid:`t0", "TracerPid: 0")) {
            ++$consecutive
            if ($consecutive -ge 8) { return }
        } else {
            $consecutive = 0
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Tracer did not remain clear for 8 consecutive samples: $last"
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $text = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Path'")
    if ($text -notmatch '^([0-9a-fA-F]{64})\s+' -or $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Path"
    }
}
function Get-GameBaseHex([int]$GamePid) {
    $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/maps'")
    $rawBases = @(foreach ($line in ($maps -split "`n")) {
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+.*libAsphalt9\.so' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) {
            [Convert]::ToUInt64($matches[1], 16)
        }
    })
    $bases = @($rawBases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
    $bases[0].ToString('x')
}
function Force-StopGame {
    for ($attempt = 0; $attempt -lt 2; ++$attempt) {
        & $AdbPath -s $Device shell "am force-stop $package" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Game force-stop failed" }
        for ($poll = 0; $poll -lt 10; ++$poll) {
            Start-Sleep -Milliseconds 100
            if ((Get-GamePid) -eq 0) { return }
        }
    }
    throw "Game remained alive after two exact package force-stop attempts"
}

foreach ($artifact in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
        (Get-Sha256 $artifact) -ne $pins[$artifact]) {
        throw "Pinned local artifact mismatch: $artifact"
    }
}
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "Lifecycle report validator selftest failed" }
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 5000) { throw "TimeoutMs must be 1000..5000" }
if ($Mode -eq 'OfflineValidateOnly') {
    $expectedCalls = if ($RecordingSourceReview) { "natural" } elseif ($SequenceReplayReview) { 3 } elseif ($ActionSchedulerReview) { 1 } else { 0 }
    Write-Output "NAL_RUNNER_OFFLINE_VALIDATION_OK semantics=$semantics device_access=0 deployed=0 action_calls=$expectedCalls"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB unavailable" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") { throw "LDPlayer ADB device unavailable" }

if ($Mode -eq 'PrepareFreshProcess') {
    if (-not $AcknowledgeFreshGameProcessRestart -or
        -not $AcknowledgePreloadWindowInjection) {
        throw "Fresh restart and preload-window injection acknowledgements are required"
    }
    $prepared = $false
    try {
        Force-StopGame
        if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
        foreach ($pair in @(
            @($payload, $remotePayload), @($candidate, $remoteCandidate),
            @($bootstrap, $remoteBootstrap), @($injector, $remoteInjector),
            @($helper, $remoteHelper))) {
            & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair[1])" }
        }
        Invoke-AdbText @('-s', $Device, 'shell',
            "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteCandidate $remoteInjector $remoteHelper'") | Out-Null
        foreach ($pair in @(
            @($remotePayload, $pins[$payload]), @($remoteCandidate, $pins[$candidate]),
            @($remoteBootstrap, $pins[$bootstrap]), @($remoteInjector, $pins[$injector]),
            @($remoteHelper, $pins[$helper]))) { Assert-RemoteHash $pair[0] $pair[1] }
        $stdout = Join-Path $outDir 'preload.stdout.txt'
        $stderr = Join-Path $outDir 'preload.stderr.txt'
        Remove-Item -LiteralPath $stdout,$stderr -Force -ErrorAction SilentlyContinue
        $preload = Start-Process -FilePath $AdbPath -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        Start-Sleep -Milliseconds 300
        Invoke-AdbText @('-s', $Device, 'shell', "am start -n $activity") | Out-Null
        if (-not $preload.WaitForExit(45000)) { Stop-Process -Id $preload.Id -Force; throw "Preload timed out" }
        $preload.WaitForExit()
        $preload.Refresh()
        $preloadExitCode = $preload.ExitCode
        $preloadText = (Get-Content -Raw $stdout) + "`n" + (Get-Content -Raw $stderr)
        $preloadText | Write-Host
        $proof = [regex]::Match($preloadText,
            '(?m)^pid=(?<pid>\d+) tid=\d+ bootstrap_verified=1 status=1 stage=2\s*$')
        if (-not $proof.Success) {
            throw "Lifecycle preload proof missing (transport exit=$preloadExitCode)"
        }
        $proofPid = [int]$proof.Groups['pid'].Value
        if ($null -eq $preloadExitCode) {
            Write-Warning "Preload transport exit code unavailable after exact bootstrap proof; requiring PID-bound mappings and TracerPid=0"
        } elseif ($preloadExitCode -ne 0) {
            Write-Warning "Preload transport exited $preloadExitCode after exact bootstrap proof; requiring PID-bound mappings and TracerPid=0"
        }
        $gamePid = 0
        $mapped = $false
        for ($attempt = 0; $attempt -lt 300; ++$attempt) {
            $observedPid = Get-GamePid
            if ($observedPid -gt 0) {
                if ($observedPid -ne $proofPid) {
                    throw "Game process changed after bootstrap proof: proved=$proofPid observed=$observedPid"
                }
                $observedMaps = ((& $AdbPath -s $Device shell "su -c 'cat /proc/$observedPid/maps'" 2>$null) -join "`n")
                if ($observedMaps.Contains($remotePayload) -and
                    $observedMaps.Contains($remoteBootstrap)) {
                    $gamePid = $observedPid
                    $mapped = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $mapped) { throw "Lifecycle payload/bootstrap did not map within 30 seconds" }
        if ($gamePid -ne $proofPid) { throw "Mapped process does not match bootstrap proof" }
        Assert-CleanTracer $gamePid
        $startTime = Get-StartTime $gamePid
        [ordered]@{version=1; semantics=$semantics; device=$Device; pid=$gamePid; start_time=$startTime;
            payload_sha256=$pins[$payload]; candidate_sha256=$pins[$candidate];
            bootstrap_sha256=$pins[$bootstrap]} | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
        $prepared = $true
        $expectedCalls = if ($RecordingSourceReview) { "natural" } elseif ($SequenceReplayReview) { 3 } elseif ($ActionSchedulerReview) { 1 } else { 0 }
        Write-Output "NAL_PREPARE_PASSED semantics=$semantics pid=$gamePid start_time=$startTime TracerPid=0 expected_action_calls=$expectedCalls"
    } finally {
        if (-not $prepared) { Force-StopGame; if (Test-Path $receipt) { Remove-Item $receipt -Force } }
    }
    return
}

if ($RecordingSourceReview) {
    throw "RecordingSourceReview execution uses run-lifecycle-source-v1.ps1 -CaptureProfile NaturalActions after preparation"
}

$requiredGates = @(
    @($AcknowledgeExactlyOneEscapeResume, 'exactly one ESC resume'),
    @($AcknowledgeOneVptrSwapAndPayloadWrites, 'one vptr swap and payload writes')
)
if ($SequenceReplayReview) {
    $requiredGates += ,@($AcknowledgeRacePausedNitroIdleUsable,
        'race paused with Nitro idle and usable')
    $requiredGates += ,@($AcknowledgeFiveFrameNaturalNitroSequence,
        'five consecutive natural action frames 0/1/0/2/0 with three total calls')
} elseif ($ActionSchedulerReview) {
    $requiredGates += ,@($AcknowledgeRacePausedNitroIdleUsable,
        'race paused with Nitro idle and usable')
    $requiredGates += ,@($AcknowledgeExactlyOneNaturalNitroActivation,
        'exactly one natural producer-thread Nitro activation with downstream state proof')
} else {
    $requiredGates += ,@($AcknowledgeAncientRuinsZl1Countdown3Paused,
        'Ancient Ruins/ZL1 countdown 3 paused')
    $requiredGates += ,@($AcknowledgeZeroActionAndNitroCalls,
        'zero action and Nitro calls')
}
$requiredGates += @(
    @($AcknowledgeFailureForceStopsFreshProcess, 'force-stop on failure'),
    @($AcknowledgePtraceStallAndCrashRisk, 'ptrace stall and crash risk')
)
foreach ($gate in $requiredGates) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared-process receipt missing" }
$confirmed = $false
$passed = $false
$mutationRisk = $false
$readyPath = ''
$armPath = ''
try {
    $prepared = Get-Content -Raw $receipt | ConvertFrom-Json
    $gamePid = Get-GamePid
    $startTime = if ($gamePid -gt 0) { Get-StartTime $gamePid } else { '' }
    if ($prepared.version -ne 1 -or $prepared.semantics -ne $semantics -or
        $prepared.device -ne $Device -or
        [int]$prepared.pid -ne $gamePid -or [string]$prepared.start_time -ne $startTime -or
        $prepared.payload_sha256 -ne $pins[$payload] -or
        $prepared.candidate_sha256 -ne $pins[$candidate] -or
        $prepared.bootstrap_sha256 -ne $pins[$bootstrap]) { throw "Prepared process mismatch" }
    $confirmed = $true
    Assert-StableCleanTracer $gamePid
    $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
    if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) { throw "Prepared mappings missing" }
    $baseHex = Get-GameBaseHex $gamePid
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
    $readyPath = "/data/local/tmp/a9tas_nal_ready_${gamePid}_$stamp"
    $armPath = "/data/local/tmp/a9tas_nal_arm_${gamePid}_$stamp"
    $remoteReport = "/data/local/tmp/a9tas_nal_${semantics}_report_${gamePid}_$stamp.bin"
    $localReport = Join-Path $root "evidence\a9tas_nal_${semantics}_report_${gamePid}_$stamp.bin"
    $markerState = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'if [ -e $readyPath ] || [ -e $armPath ]; then echo EXISTS; else echo ABSENT; fi'")
    if ($markerState -ne 'ABSENT') { throw "READY/ARM marker collision" }
    $remoteCommand = "$remoteCandidate $gamePid $startTime $baseHex $TimeoutMs $remoteReport $readyPath $armPath $ack"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $AdbPath; $startInfo.UseShellExecute = $false; $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true; $startInfo.RedirectStandardError = $true
    if ($null -ne $startInfo.ArgumentList) {
        foreach ($argument in @('-s', $Device, 'shell', "su -c '$remoteCommand'")) {
            $startInfo.ArgumentList.Add($argument)
        }
    } else {
        # Windows PowerShell 5.1 exposes ProcessStartInfo but not the modern
        # ArgumentList collection. All interpolated tokens were validated or
        # generated by this runner; quote the single remote shell argument.
        $startInfo.Arguments = "-s $Device shell `"su -c '$remoteCommand'`""
    }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Candidate launch failed" }
    $ready = $false
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $readyDeadline -and -not $process.HasExited) {
        $exists = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'if [ -e $readyPath ]; then echo READY; fi'")
        if ($exists -eq 'READY') { $ready = $true; break }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) {
        if (-not $process.HasExited) { $process.WaitForExit(1000) | Out-Null }
        $earlyOut = $process.StandardOutput.ReadToEnd()
        $earlyErr = $process.StandardError.ReadToEnd()
        throw "Candidate did not reach READY_NO_ATTACH exit=$($process.ExitCode) stdout=[$earlyOut] stderr=[$earlyErr]"
    }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
    if ($readyText -notmatch '^NAL_READY_NO_ATTACH ' -or
        $readyText -notmatch 'paused_zero_samples=8 target_threads_attached=0 game_writes=0') {
        throw "READY_NO_ATTACH proof mismatch"
    }
    Assert-StableCleanTracer $gamePid
    Write-Host "NAL_HOST_READY pid=$gamePid TracerPid=0 sending_single_ESC_then_ARM=1"
    # The composite command may resume the game even if ARM publication later
    # fails. Mark risk before issuing it so every uncertain post-ESC outcome
    # force-stops this exact prepared process.
    $mutationRisk = $true
    $resumeAndArm = "input keyevent 111 && echo NAL_ARM $gamePid $startTime > $armPath"
    & $AdbPath -s $Device shell "su -c '$resumeAndArm'" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Single ESC plus ARM publication failed" }
    $waitMultiplier = if ($SequenceReplayReview) { 8 } else { 4 }
    if (-not $process.WaitForExit($waitMultiplier * $TimeoutMs + 10000)) {
        throw "Lifecycle candidate timed out"
    }
    $stdoutText = $process.StandardOutput.ReadToEnd(); $stderrText = $process.StandardError.ReadToEnd()
    if ($stdoutText) { $stdoutText.TrimEnd() | Write-Host }
    if ($stderrText) { $stderrText.TrimEnd() | Write-Warning }
    if ($process.ExitCode -ne 0 -or $stdoutText -notmatch 'NAL_DONE success=1') { throw "Lifecycle candidate failed closed" }
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTime $gamePid) -ne $startTime) { throw "Game process changed" }
    Assert-CleanTracer $gamePid
    Invoke-AdbText @('-s', $Device, 'shell', "su -c 'chmod 644 $remoteReport'") | Out-Null
    & $AdbPath -s $Device pull $remoteReport $localReport | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Report pull failed" }
    & $python.Source $validator $localReport
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle report validation failed" }
    Remove-Item -LiteralPath $receipt -Force
    $passed = $true
    $receiptKind = if ($SequenceReplayReview) {
        "action_receipt=1 frames=5 sequence=0_1_0_2_0 action_calls=3"
    } elseif ($ActionSchedulerReview) {
        "action_receipt=1 activation_count=1"
    } else {
        "zero_call_receipt=1"
    }
    Write-Output "NAL_LIVE_GATE_PASSED semantics=$semantics pid=$gamePid single_ESC=1 positive_delta_self_observed=1 $receiptKind natural_removal=1 TracerPid=0"
    Write-Output "report=$localReport"
} finally {
    if ($readyPath -ne '') { & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" 2>$null | Out-Null }
    if ($armPath -ne '') { & $AdbPath -s $Device shell "su -c 'rm -f $armPath'" 2>$null | Out-Null }
    if ($confirmed -and -not $passed -and $mutationRisk) {
        Force-StopGame
        if (Test-Path $receipt) { Remove-Item $receipt -Force }
    } elseif ($confirmed -and -not $passed) {
        Write-Warning "Read-only preflight failed before ESC; prepared game process and receipt were preserved"
    }
}
