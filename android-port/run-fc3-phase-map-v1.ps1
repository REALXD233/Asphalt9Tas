param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ProbePreparedProcess")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 1500,
    [int]$ReceiptTtlSeconds = 1800,
    [int]$PostProbeObservationMs = 3000,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgePassiveFc2PayloadOnly,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeFc2Fc3PhaseMap,
    [switch]$AcknowledgeOneVptrSwapAndGameOwnedAddRemove,
    [switch]$AcknowledgeNoActionInputNitroOrPhysicsWrite,
    [switch]$AcknowledgeFourHardwareWatchpointsAndPtraceStalls,
    [switch]$AcknowledgeExitKillMayTerminateFreshProcess,
    [switch]$AcknowledgeFailureForceStopsFreshProcess
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\fc3-phase-map-candidate-v1"
$payload = Join-Path $root "build\frame-callback-deferred-registration-v1\liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$bootstrap = Join-Path $root "build\fc2-runner-v1\liba9tas_bootstrap_frame_callback_fc2_v1.so"
$controller = Join-Path $outDir "a9tas_fc3_phase_map_controller_v1"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_fc2_frame_callback_preload_v1.sh"
$fc2Validator = Join-Path $root "tools\validate_fc2_report_v1.py"
$phaseValidator = Join-Path $root "tools\validate_fc3_phase_map_v1.py"
$receipt = Join-Path $outDir "fc3-phase-map-prepared-v1.json"

$pins = @{
    $payload = "550fc3e579d732a9f570ea869eb487161ed2e95c3540fc67c8f6785e7abbcefb"
    $bootstrap = "e8ea8c3a6da1c38d0a3f4fdf9a388838e71afa7ac61544567babb2e0088dc993"
    $controller = "3a2a3a0077638ef146726b4f2e727e5de9d6092619d33d925c6d4d6686b3e999"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "d03f4ddea2412d4ed11e18064e5c8102a04fa7718d57fec050faff1a89109b10"
    $fc2Validator = "acce3c4f1fa4cf4162a0b186025a327960a1ac0f006a7634f02bc45c1fae7df3"
    $phaseValidator = "6b4dfa652ab24da240187e88593c22979a4da433fb422298aa676d53fdf0de1a"
}

$remotePayload = "/data/local/tmp/liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_frame_callback_fc2_v1.so"
$remoteController = "/data/local/tmp/a9tas_fc3_phase_map_controller_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_fc2_v1"
$remoteHelper = "/data/local/tmp/run_fc3_phase_map_preload_v1.sh"
$controllerAck = "I_ACCEPT_FC3_PHASE_MAP_OBSERVE_ONLY_V1"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Missing pinned phase-map artifact: $artifact"
        }
        if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Pinned phase-map artifact hash mismatch: $artifact"
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
    $fields = @($matches[1] -split '\s+')
    if ($fields.Count -lt 19 -or $fields[18] -notmatch '^\d+$') {
        throw "Process start-time read failed"
    }
    return $fields[18]
}

function Get-BootId {
    $value = ((& $AdbPath -s $Device shell 'cat /proc/sys/kernel/random/boot_id' 2>$null) -join '').Trim()
    if ($value -notmatch '^[0-9a-fA-F-]{36}$') { throw "Device boot ID read failed" }
    return $value.ToLowerInvariant()
}

function Assert-CleanTracer([int]$GamePid) {
    $line = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$GamePid/status'" 2>$null) -join '').Trim()
    if ($line -ne "TracerPid:`t0" -and $line -ne "TracerPid: 0") {
        throw "Game process has an active tracer: $line"
    }
}

function Get-RemoteHash([string]$RemotePath) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $RemotePath'" 2>$null) -join '').Trim()
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "Device hash read failed: $RemotePath"
    }
    return $matches[1].ToLowerInvariant()
}

function Assert-RemoteHash([string]$RemotePath, [string]$Expected) {
    if ((Get-RemoteHash $RemotePath) -ne $Expected) {
        throw "Device artifact hash mismatch: $RemotePath"
    }
}

function Assert-NoStalePreloadWaiter {
    $lines = & $AdbPath -s $Device shell "su -c 'ps -A -o PID,ARGS'" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Unable to audit stale preload waiters" }
    $text = $lines -join "`n"
    if ($text.Contains($remoteInjector) -or $text.Contains($remoteHelper)) {
        throw "Stale phase-map preload waiter detected; restart the emulator"
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

function Stop-FreshPackage {
    & $AdbPath -s $Device shell "am force-stop $package" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Fresh package force-stop failed" }
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Fresh package remained alive after force-stop" }
}

Assert-LocalArtifacts
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $fc2Validator --selftest
if ($LASTEXITCODE -ne 0) { throw "FC-2 validator selftest failed" }
& $python.Source $phaseValidator --selftest
if ($LASTEXITCODE -ne 0) { throw "Phase-map validator selftest failed" }
if ($TimeoutMs -lt 250 -or $TimeoutMs -gt 3000) { throw "TimeoutMs must be 250..3000" }
if ($ReceiptTtlSeconds -lt 60 -or $ReceiptTtlSeconds -gt 3600) { throw "ReceiptTtlSeconds must be 60..3600" }
if ($PostProbeObservationMs -lt 1000 -or $PostProbeObservationMs -gt 10000) { throw "PostProbeObservationMs must be 1000..10000" }

if ($Mode -eq "OfflineValidateOnly") {
    Write-Output "FC3_PHASE_MAP_RUNNER_OFFLINE_OK device_access=0 restart=0 attached=0 game_writes=0"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }

if ($Mode -eq "PrepareFreshProcess") {
    foreach ($gate in @(
        @($AcknowledgeFreshGameProcessRestart, "fresh game process restart"),
        @($AcknowledgePreloadWindowInjection, "preload-window injection"),
        @($AcknowledgePassiveFc2PayloadOnly, "passive FC-2 payload")
    )) {
        if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
    }
    $prepared = $false
    try {
        Stop-FreshPackage
        if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
        Assert-NoStalePreloadWaiter
        foreach ($pair in @(
            @($payload, $remotePayload), @($bootstrap, $remoteBootstrap),
            @($controller, $remoteController), @($injector, $remoteInjector),
            @($helper, $remoteHelper)
        )) {
            Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) "push $($pair[1])" | Out-Null
        }
        Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteInjector $remoteHelper'") "device chmod" | Out-Null
        foreach ($pair in @(
            @($remotePayload, $pins[$payload]), @($remoteBootstrap, $pins[$bootstrap]),
            @($remoteController, $pins[$controller]), @($remoteInjector, $pins[$injector]),
            @($remoteHelper, $pins[$helper])
        )) { Assert-RemoteHash $pair[0] $pair[1] }

        $stdout = Join-Path $outDir "fc3-phase-map-preload.stdout.txt"
        $stderr = Join-Path $outDir "fc3-phase-map-preload.stderr.txt"
        Set-Content -LiteralPath $stdout -Value '' -NoNewline
        Set-Content -LiteralPath $stderr -Value '' -NoNewline
        $preload = Start-Process -FilePath $AdbPath -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        Start-Sleep -Milliseconds 300
        Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") "start fresh game process" | Out-Null
        if (-not $preload.WaitForExit(45000)) {
            Stop-Process -Id $preload.Id -Force
            throw "Phase-map preload injector timed out"
        }
        $preloadText = (Get-Content -Raw -LiteralPath $stdout) + "`n" + (Get-Content -Raw -LiteralPath $stderr)
        $preloadText | Write-Host
        if ($preload.ExitCode -ne 0 -or $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
            throw "Phase-map preload bootstrap did not arm"
        }
        Assert-NoStalePreloadWaiter
        $gamePid = 0
        $mapped = $false
        for ($attempt = 0; $attempt -lt 300; $attempt++) {
            $gamePid = Get-GamePid
            if ($gamePid -gt 0) {
                $maps = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
                if ($maps.Contains($remotePayload) -and $maps.Contains($remoteBootstrap)) { $mapped = $true; break }
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $mapped) { throw "Prepared phase-map modules were not mapped" }
        Assert-CleanTracer $gamePid
        $startTime = Get-ProcessStartTime $gamePid
        $bootId = Get-BootId
        $nonce = [Guid]::NewGuid().ToString("N")
        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $tempReceipt = "$receipt.$nonce.tmp"
        [ordered]@{
            version = 1; device = $Device; boot_id = $bootId; pid = $gamePid
            start_time = $startTime; nonce = $nonce; created_unix = $now
            payload_sha256 = $pins[$payload]; bootstrap_sha256 = $pins[$bootstrap]
            controller_sha256 = $pins[$controller]; injector_sha256 = $pins[$injector]
            helper_sha256 = $pins[$helper]; fc2_validator_sha256 = $pins[$fc2Validator]
            phase_validator_sha256 = $pins[$phaseValidator]
        } | ConvertTo-Json | Set-Content -LiteralPath $tempReceipt -Encoding utf8
        Move-Item -LiteralPath $tempReceipt -Destination $receipt
        Write-Output "FC3_PHASE_MAP_PREPARED pid=$gamePid start_time=$startTime nonce=$nonce device_access=1 attached=0"
        $prepared = $true
    } finally {
        if (-not $prepared) {
            try { Stop-FreshPackage } finally {
                if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
            }
        }
    }
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeFc2Fc3PhaseMap, "the FC-2 to FC-3 phase-map transaction"),
    @($AcknowledgeOneVptrSwapAndGameOwnedAddRemove, "one vptr swap and game-owned add/remove"),
    @($AcknowledgeNoActionInputNitroOrPhysicsWrite, "zero action, input, Nitro and physics writes"),
    @($AcknowledgeFourHardwareWatchpointsAndPtraceStalls, "four hardware watchpoints and ptrace stalls"),
    @($AcknowledgeExitKillMayTerminateFreshProcess, "PTRACE EXITKILL may terminate the fresh process"),
    @($AcknowledgeFailureForceStopsFreshProcess, "force-stop on any probe failure")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared-process receipt missing" }

$cleanupArmed = $true
$probePassed = $false
$gamePid = 0
try {
    $prepared = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $gamePid = Get-GamePid
    $startTime = if ($gamePid -gt 0) { Get-ProcessStartTime $gamePid } else { "" }
    $bootId = Get-BootId
    if ($prepared.version -ne 1 -or $prepared.device -ne $Device -or
        $prepared.boot_id -ne $bootId -or [int]$prepared.pid -ne $gamePid -or
        [string]$prepared.start_time -ne $startTime -or
        [string]$prepared.nonce -notmatch '^[0-9a-f]{32}$' -or
        $now -lt [long]$prepared.created_unix -or
        $now - [long]$prepared.created_unix -gt $ReceiptTtlSeconds -or
        $prepared.payload_sha256 -ne $pins[$payload] -or
        $prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or
        $prepared.controller_sha256 -ne $pins[$controller] -or
        $prepared.injector_sha256 -ne $pins[$injector] -or
        $prepared.helper_sha256 -ne $pins[$helper] -or
        $prepared.fc2_validator_sha256 -ne $pins[$fc2Validator] -or
        $prepared.phase_validator_sha256 -ne $pins[$phaseValidator]) {
        throw "Prepared phase-map receipt mismatch or expiry"
    }
    Assert-CleanTracer $gamePid
    foreach ($pair in @(
        @($remotePayload, $pins[$payload]), @($remoteBootstrap, $pins[$bootstrap]),
        @($remoteController, $pins[$controller])
    )) { Assert-RemoteHash $pair[0] $pair[1] }
    $maps = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
    if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
        throw "Prepared phase-map modules are not mapped"
    }
    $baseHex = Get-LibraryBaseHex $gamePid
    $nonce = [string]$prepared.nonce
    $remoteFc2 = "/data/local/tmp/a9tas_fc2_phase_${gamePid}_${nonce}.bin"
    $remotePhase = "/data/local/tmp/a9tas_fc3_phase_${gamePid}_${nonce}.bin"
    $localFc2 = Join-Path $outDir "a9tas_fc2_phase_${gamePid}_${nonce}.bin"
    $localPhase = Join-Path $outDir "a9tas_fc3_phase_${gamePid}_${nonce}.bin"
    if ((Test-Path -LiteralPath $localFc2) -or (Test-Path -LiteralPath $localPhase)) { throw "Local report already exists" }
    foreach ($remote in @($remoteFc2, $remotePhase)) {
        $exists = ((& $AdbPath -s $Device shell "su -c 'if [ -e $remote ]; then echo EXISTS; fi'" 2>$null) -join '').Trim()
        if ($exists) { throw "Remote report already exists: $remote" }
    }
    if ((Get-GamePid) -ne $gamePid -or (Get-ProcessStartTime $gamePid) -ne $startTime) { throw "Process changed before attach" }
    Assert-CleanTracer $gamePid
    $lines = & $AdbPath -s $Device shell `
        "su -c '$remoteController $gamePid $startTime $baseHex $TimeoutMs $remoteFc2 $remotePhase $controllerAck'" 2>&1
    $code = $LASTEXITCODE
    $lines | Out-Host
    $text = $lines -join "`n"
    $controllerPassed = $code -eq 0 -and $text -match 'FC2_DONE success=1' -and
        $text -match 'FC3_PHASE_MAP_DONE success=1'

    # Reports are created by the root controller as mode 0600.  Make only
    # these nonce-bound files pull-readable, then preserve and hash them before
    # deciding whether the controller passed.  A failed live attempt must not
    # discard the evidence needed to distinguish transport failure from a
    # rejected phase hypothesis.
    foreach ($remote in @($remoteFc2, $remotePhase)) {
        Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c 'chmod 0444 $remote'") "make controller report pull-readable" | Out-Null
    }
    $remoteFc2Hash = Get-RemoteHash $remoteFc2
    $remotePhaseHash = Get-RemoteHash $remotePhase
    Invoke-AdbChecked @('-s', $Device, 'pull', $remoteFc2, $localFc2) "pull FC-2 report" | Out-Null
    Invoke-AdbChecked @('-s', $Device, 'pull', $remotePhase, $localPhase) "pull phase-map report" | Out-Null
    if ((Get-Sha256 $localFc2) -ne $remoteFc2Hash -or
        (Get-Sha256 $localPhase) -ne $remotePhaseHash) {
        throw "Remote/local report hash mismatch"
    }
    if (-not $controllerPassed) {
        Write-Output "FC3_PHASE_MAP_FAILED_REPORTS_PRESERVED fc2=$localFc2 phase=$localPhase"
        throw "Phase-map controller failed closed"
    }
    & $python.Source $fc2Validator $localFc2 --pid $gamePid --base "0x$baseHex"
    if ($LASTEXITCODE -ne 0) { throw "FC-2 report validation failed" }
    & $python.Source $phaseValidator $localPhase --fc2-report $localFc2
    if ($LASTEXITCODE -ne 0) { throw "Phase-map report validation failed" }
    if ((Get-GamePid) -ne $gamePid -or (Get-ProcessStartTime $gamePid) -ne $startTime) { throw "Process changed after probe" }
    Assert-CleanTracer $gamePid
    Start-Sleep -Milliseconds $PostProbeObservationMs
    if ((Get-GamePid) -ne $gamePid -or (Get-ProcessStartTime $gamePid) -ne $startTime) { throw "Process changed during survival check" }
    Assert-CleanTracer $gamePid
    Remove-Item -LiteralPath $receipt -Force
    $probePassed = $true
} finally {
    if ($cleanupArmed -and -not $probePassed) {
        try { Stop-FreshPackage } finally {
            if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
        }
    }
}
Write-Output "FC3_PHASE_MAP_PROBE_PASS pid=$gamePid phase_order=DT-C98-open-C9C-F64-dedicated-close-world-nextDT-nextC98 game_writes=1 additional_game_writes=0 action_calls=0 nitro_calls=0 physics_writes=0 TracerPid=0"
