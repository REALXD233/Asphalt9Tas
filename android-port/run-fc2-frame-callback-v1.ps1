param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ProbePreparedProcess")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 1500,
    [int]$PostProbeObservationMs = 3000,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgePassiveFc2PayloadOnly,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeThreeFrameDeferredRegistration,
    [switch]$AcknowledgeOneVptrSwapAndGameOwnedAddRemove,
    [switch]$AcknowledgeNoActionInputNitroOrPhysicsCorrection,
    [switch]$AcknowledgeProbeFailureForceStopsFreshProcess,
    [switch]$AcknowledgeShortPtraceStallAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\fc2-runner-v1"
$payload = Join-Path $root "build\frame-callback-deferred-registration-v1\liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_frame_callback_fc2_v1.so"
$controller = Join-Path $outDir "a9tas_fc2_frame_callback_controller_v1"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_fc2_frame_callback_preload_v1.sh"
$reportValidator = Join-Path $root "tools\validate_fc2_report_v1.py"
$runnerPolicy = Join-Path $root "tools\test_run_fc2_frame_callback_policy_v1.py"
$receipt = Join-Path $outDir "fc2-prepared-process-v1.json"

$pins = @{
    $payload = "550fc3e579d732a9f570ea869eb487161ed2e95c3540fc67c8f6785e7abbcefb"
    $bootstrap = "e8ea8c3a6da1c38d0a3f4fdf9a388838e71afa7ac61544567babb2e0088dc993"
    $controller = "d3c21c66047a5d64e56a44d55c4ca259b8555905361749103c30dcc6f570f443"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "d03f4ddea2412d4ed11e18064e5c8102a04fa7718d57fec050faff1a89109b10"
    $reportValidator = "acce3c4f1fa4cf4162a0b186025a327960a1ac0f006a7634f02bc45c1fae7df3"
}

$remotePayload = "/data/local/tmp/liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_frame_callback_fc2_v1.so"
$remoteController = "/data/local/tmp/a9tas_fc2_frame_callback_controller_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_fc2_v1"
$remoteHelper = "/data/local/tmp/run_fc2_frame_callback_preload_v1.sh"
$controllerAck = "I_ACCEPT_FC2_THREE_FRAME_DEFERRED_REGISTRATION_OBSERVE_ONLY_V1"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Missing reviewed FC-2 artifact: $artifact"
        }
        if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Reviewed FC-2 artifact hash mismatch: $artifact"
        }
    }
}

function Invoke-AdbChecked([string[]]$Arguments, [string]$Description) {
    $lines = & $AdbPath @Arguments 2>&1
    $code = $LASTEXITCODE
    $lines | Out-Host
    if ($code -ne 0) { throw "$Description failed (exit=$code)" }
    return $lines
}

function Get-GamePid {
    $text = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
    if ($text -notmatch '^\d+$') { return 0 }
    return [int]$text
}

function Get-ProcessStartTime([int]$GamePid) {
    $text = ((& $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/stat'" 2>$null) -join '').Trim()
    if ($text -notmatch '^\d+\s+\(.*\)\s+\S\s+(.+)$') {
        throw "Process stat read failed"
    }
    $fieldsAfterState = @($matches[1] -split '\s+')
    if ($fieldsAfterState.Count -lt 19 -or $fieldsAfterState[18] -notmatch '^\d+$') {
        throw "Process start-time read failed"
    }
    return $fieldsAfterState[18]
}

function Assert-CleanTracer([int]$GamePid) {
    $line = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$GamePid/status'" 2>$null) -join '').Trim()
    if ($line -ne "TracerPid:`t0" -and $line -ne "TracerPid: 0") {
        throw "Game process has an active tracer: $line"
    }
}

function Assert-RemoteHash([string]$RemotePath, [string]$Expected) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $RemotePath'" 2>$null) -join '').Trim()
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Device artifact hash mismatch: $RemotePath"
    }
}

function Assert-NoStaleFc2Waiter {
    $lines = & $AdbPath -s $Device shell "su -c 'ps -A -o PID,ARGS'" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Unable to audit stale FC-2 preload waiters" }
    $text = $lines -join "`n"
    if ($text.Contains($remoteInjector) -or $text.Contains($remoteHelper)) {
        throw "Stale FC-2 preload waiter detected; restart the emulator before preparing a new process"
    }
}

function Get-LibraryBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $bases = foreach ($line in $maps) {
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+.*libAsphalt9\.so' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) {
            [Convert]::ToUInt64($matches[1], 16)
        }
    }
    $bases = @($bases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
    return $bases[0].ToString("x")
}

function Stop-ConfirmedFreshProcess([int]$GamePid, [string]$StartTime) {
    $current = Get-GamePid
    # Once the receipt was confirmed, a changed PID/start-time is itself a
    # probe failure.  Always put the package in the stopped state, even if the
    # failed process is momentarily absent and could otherwise auto-restart.
    if ($current -eq $GamePid) {
        $currentStart = Get-ProcessStartTime $current
        if ($currentStart -ne $StartTime) {
            Write-Warning "Confirmed FC-2 PID was reused before force-stop"
        }
    }
    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") `
        "force-stop failed FC-2 fresh process" | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) {
        throw "Failed FC-2 package remained alive after force-stop"
    }
}

Assert-LocalArtifacts
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $reportValidator --selftest
if ($LASTEXITCODE -ne 0) { throw "FC-2 report validator selftest failed" }
& $python.Source $runnerPolicy
if ($LASTEXITCODE -ne 0) { throw "FC-2 runner policy failed" }
if ($TimeoutMs -lt 250 -or $TimeoutMs -gt 3000) {
    throw "TimeoutMs must be 250..3000"
}
if ($PostProbeObservationMs -lt 1000 -or $PostProbeObservationMs -gt 10000) {
    throw "PostProbeObservationMs must be 1000..10000"
}

if ($Mode -eq "OfflineValidateOnly") {
    Write-Output "FC2_RUNNER_OFFLINE_VALIDATION_OK device_access=0 restart=0 attached=0 game_writes=0 registration=0 removal=0"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

if ($Mode -eq "PrepareFreshProcess") {
    foreach ($gate in @(
        @($AcknowledgeFreshGameProcessRestart, "fresh game process restart"),
        @($AcknowledgePreloadWindowInjection, "preload-window injection"),
        @($AcknowledgePassiveFc2PayloadOnly, "passive FC-2 payload with no guest export call")
    )) {
        if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
    }

    $preparePassed = $false
    try {

    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") `
        "force-stop old game process" | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    if (Test-Path -LiteralPath $receipt) {
        Remove-Item -LiteralPath $receipt -Force
    }
    Assert-NoStaleFc2Waiter

    foreach ($pair in @(
        @($payload, $remotePayload), @($bootstrap, $remoteBootstrap),
        @($controller, $remoteController), @($injector, $remoteInjector),
        @($helper, $remoteHelper)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) `
            "push $($pair[1])" | Out-Null
    }
    Invoke-AdbChecked @(
        '-s', $Device, 'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteInjector $remoteHelper'"
    ) "device chmod" | Out-Null
    foreach ($pair in @(
        @($remotePayload, $pins[$payload]), @($remoteBootstrap, $pins[$bootstrap]),
        @($remoteController, $pins[$controller]), @($remoteInjector, $pins[$injector]),
        @($remoteHelper, $pins[$helper])
    )) { Assert-RemoteHash $pair[0] $pair[1] }

    $stdout = Join-Path $outDir "fc2-preload.stdout.txt"
    $stderr = Join-Path $outDir "fc2-preload.stderr.txt"
    Set-Content -LiteralPath $stdout -Value '' -NoNewline
    Set-Content -LiteralPath $stderr -Value '' -NoNewline
    $preload = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 300
    Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") `
        "start fresh game process" | Out-Null
    if (-not $preload.WaitForExit(45000)) {
        Stop-Process -Id $preload.Id -Force
        throw "FC-2 preload injector timed out"
    }
    $preloadText = ((Get-Content -Raw -LiteralPath $stdout) + "`n" +
                    (Get-Content -Raw -LiteralPath $stderr))
    $preloadText | Write-Host
    if ($preload.ExitCode -ne 0 -or
        $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "FC-2 preload bootstrap did not reach armed stage"
    }
    Assert-NoStaleFc2Waiter

    $gamePid = 0
    $mapped = $false
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $gamePid = Get-GamePid
        if ($gamePid -gt 0) {
            $maps = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
            if ($maps.Contains($remotePayload) -and $maps.Contains($remoteBootstrap)) {
                $mapped = $true
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped) { throw "FC-2 payload/bootstrap did not map in the fresh process" }
    Assert-CleanTracer $gamePid
    $startTime = Get-ProcessStartTime $gamePid
    [ordered]@{
        version = 1; device = $Device; pid = $gamePid; start_time = $startTime
        payload_sha256 = $pins[$payload]; bootstrap_sha256 = $pins[$bootstrap]
        controller_sha256 = $pins[$controller]; injector_sha256 = $pins[$injector]
        helper_sha256 = $pins[$helper]
    } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    Write-Output "FC2_PREPARE_FRESH_PROCESS_PASSED pid=$gamePid start_time=$startTime called=0 attached=0 game_writes=0 registration=0 removal=0 TracerPid=0"
    $preparePassed = $true
    } finally {
        if (-not $preparePassed) {
            try {
                & $AdbPath -s $Device shell "am force-stop $package" 2>$null | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    throw "Unable to force-stop package after failed FC-2 preparation"
                }
            } finally {
                if (Test-Path -LiteralPath $receipt) {
                    Remove-Item -LiteralPath $receipt -Force
                }
            }
        }
    }
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeThreeFrameDeferredRegistration, "the three-frame deferred-registration observer"),
    @($AcknowledgeOneVptrSwapAndGameOwnedAddRemove, "one vptr swap and game-owned add/remove mutations"),
    @($AcknowledgeNoActionInputNitroOrPhysicsCorrection, "zero action, input, Nitro and physics correction"),
    @($AcknowledgeProbeFailureForceStopsFreshProcess, "force-stop of this fresh process on any probe failure"),
    @($AcknowledgeShortPtraceStallAndCrashRisk, "the short ptrace stall and crash risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) {
    throw "Prepared-process receipt missing"
}

$confirmedFresh = $false
$probePassed = $false
$gamePid = 0
$startTime = ""
try {
    $prepared = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
    $gamePid = Get-GamePid
    $startTime = if ($gamePid -gt 0) { Get-ProcessStartTime $gamePid } else { "" }
    if ($prepared.version -ne 1 -or $prepared.device -ne $Device -or
        [int]$prepared.pid -ne $gamePid -or
        [string]$prepared.start_time -ne $startTime -or
        $prepared.payload_sha256 -ne $pins[$payload] -or
        $prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or
        $prepared.controller_sha256 -ne $pins[$controller] -or
        $prepared.injector_sha256 -ne $pins[$injector] -or
        $prepared.helper_sha256 -ne $pins[$helper]) {
        throw "Prepared process receipt mismatch"
    }
    $confirmedFresh = $true
    Assert-CleanTracer $gamePid
    foreach ($pair in @(
        @($remotePayload, $pins[$payload]), @($remoteBootstrap, $pins[$bootstrap]),
        @($remoteController, $pins[$controller])
    )) { Assert-RemoteHash $pair[0] $pair[1] }
    $maps = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
    if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
        throw "Prepared FC-2 modules are not mapped in this process"
    }

    $baseHex = Get-LibraryBaseHex $gamePid
    $remoteReport = "/data/local/tmp/a9tas_fc2_report_${gamePid}_${startTime}.bin"
    $localReport = Join-Path $outDir "a9tas_fc2_report_${gamePid}_${startTime}.bin"
    if (Test-Path -LiteralPath $localReport) { throw "Local report already exists" }
    $remoteExists = ((& $AdbPath -s $Device shell "su -c 'if [ -e $remoteReport ]; then echo EXISTS; fi'" 2>$null) -join '').Trim()
    if ($remoteExists -ne '') { throw "Remote report already exists" }

    $lines = & $AdbPath -s $Device shell `
        "su -c '$remoteController $gamePid $baseHex $TimeoutMs $remoteReport $controllerAck'" 2>&1
    $code = $LASTEXITCODE
    $lines | Out-Host
    $text = $lines -join "`n"
    if ($code -ne 0 -or $text -notmatch "FC2_DONE success=1") {
        throw "FC-2 three-frame controller failed closed"
    }
    Invoke-AdbChecked @('-s', $Device, 'pull', $remoteReport, $localReport) `
        "pull FC-2 report" | Out-Null
    $validationLines = & $python.Source $reportValidator $localReport `
        --pid $gamePid --base "0x$baseHex"
    $validationCode = $LASTEXITCODE
    $validationLines | Out-Host
    if ($validationCode -ne 0 -or
        ($validationLines -join "`n") -notmatch "FC2_REPORT_VALID passed=1 pid=$gamePid base=0x$baseHex") {
        throw "FC-2 report validation failed"
    }
    if ((Get-GamePid) -ne $gamePid -or
        (Get-ProcessStartTime $gamePid) -ne $startTime) {
        throw "Game process changed after FC-2"
    }
    Assert-CleanTracer $gamePid
    Start-Sleep -Milliseconds $PostProbeObservationMs
    if ((Get-GamePid) -ne $gamePid -or
        (Get-ProcessStartTime $gamePid) -ne $startTime) {
        throw "Game process changed during delayed FC-2 survival observation"
    }
    Assert-CleanTracer $gamePid
    if (Test-Path -LiteralPath $receipt) {
        Remove-Item -LiteralPath $receipt -Force
    }
    $probePassed = $true
} finally {
    if ($confirmedFresh -and -not $probePassed) {
        try {
            Stop-ConfirmedFreshProcess $gamePid $startTime
        } finally {
            if (Test-Path -LiteralPath $receipt) {
                Remove-Item -LiteralPath $receipt -Force
            }
        }
    }
}
Write-Output "FC2_PROBE_PASSED pid=$gamePid start_time=$startTime three_frame=1 membership=0,1,0 game_writes=1 game_owned_add=1 game_owned_remove=1 action_calls=0 input=0 nitro=0 physics_correction=0 delayed_observation_ms=$PostProbeObservationMs TracerPid=0"
