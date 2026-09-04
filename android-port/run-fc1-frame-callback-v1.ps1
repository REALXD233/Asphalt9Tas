param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ProbePreparedProcess")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 1000,
    [int]$PostProbeObservationMs = 3000,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgePassiveFc0PayloadOnly,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeOneFramePassthroughVptrSwap,
    [switch]$AcknowledgeNoActionInputNitroOrPhysicsCorrection,
    [switch]$AcknowledgeShortPtraceStallAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\fc1-runner-v1"
$payload = Join-Path $root "build\frame-callback-bootstrap-v1\liba9tas_frame_callback_bootstrap_v1_build_only.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_frame_callback_v1.so"
$controller = Join-Path $outDir "a9tas_fc1_frame_callback_controller_v1"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_fc1_frame_callback_preload_v1.sh"
$reportValidator = Join-Path $root "tools\validate_fc1_report_v1.py"
$runnerPolicy = Join-Path $root "tools\test_run_fc1_frame_callback_policy_v1.py"
$receipt = Join-Path $outDir "fc1-prepared-process-v1.json"

$pins = @{
    $payload = "1223a5104a05688c8dda4ec7f5454cc42fe23a6f21886fccf528a32a21633298"
    $bootstrap = "700bb73d148e57939e0e94758446c078e423f72337c907053a89bca030c55892"
    $controller = "47e4504c28cb59e66a0560d8680ca62e47cb96f5ed3a2fd6ae4b13ea6a3b5ee3"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "9a8a8ee0eaf763bf07f11baf1f6f3303284b1bc2b82fe89526c40104c8763715"
    $reportValidator = "51c2cedee14267e5e8f3342890a3343525b76789bdc4f560159c67bf28b869ac"
}

$remotePayload = "/data/local/tmp/liba9tas_frame_callback_bootstrap_v1_build_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_frame_callback_v1.so"
$remoteController = "/data/local/tmp/a9tas_fc1_frame_callback_controller_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_fc1_v1"
$remoteHelper = "/data/local/tmp/run_fc1_frame_callback_preload_v1.sh"
$controllerAck = "I_ACCEPT_FC1_ONE_FRAME_PASSTHROUGH_NO_ACTION_V1"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Missing reviewed FC-1 artifact: $artifact"
        }
        if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Reviewed FC-1 artifact hash mismatch: $artifact"
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

function Assert-NoStaleFc1Waiter {
    $lines = & $AdbPath -s $Device shell "su -c 'ps -A -o PID,ARGS'" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Unable to audit stale FC-1 preload waiters" }
    $text = $lines -join "`n"
    if ($text.Contains($remoteInjector) -or $text.Contains($remoteHelper)) {
        throw "Stale FC-1 preload waiter detected; restart the emulator before preparing a new process"
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

Assert-LocalArtifacts
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $reportValidator --selftest
if ($LASTEXITCODE -ne 0) { throw "FC-1 report validator selftest failed" }
& $python.Source $runnerPolicy
if ($LASTEXITCODE -ne 0) { throw "FC-1 runner policy failed" }
if ($TimeoutMs -lt 100 -or $TimeoutMs -gt 2000) {
    throw "TimeoutMs must be 100..2000"
}
if ($PostProbeObservationMs -lt 1000 -or $PostProbeObservationMs -gt 10000) {
    throw "PostProbeObservationMs must be 1000..10000"
}

if ($Mode -eq "OfflineValidateOnly") {
    Write-Output "FC1_RUNNER_OFFLINE_VALIDATION_OK device_access=0 restart=0 attached=0 game_writes=0"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

if ($Mode -eq "PrepareFreshProcess") {
    foreach ($gate in @(
        @($AcknowledgeFreshGameProcessRestart, "fresh game process restart"),
        @($AcknowledgePreloadWindowInjection, "preload-window injection"),
        @($AcknowledgePassiveFc0PayloadOnly, "passive FC-0 payload with no guest export call")
    )) {
        if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
    }

    # Stop any process that could still map the previous remote payload before
    # replacing that file.  Pushing over a live mapped ELF can corrupt the old
    # mapping even though the process is about to be restarted.
    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") "force-stop old game process" | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    Assert-NoStaleFc1Waiter

    foreach ($pair in @(
        @($payload, $remotePayload), @($bootstrap, $remoteBootstrap),
        @($controller, $remoteController), @($injector, $remoteInjector),
        @($helper, $remoteHelper)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) "push $($pair[1])" | Out-Null
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

    $stdout = Join-Path $outDir "fc1-preload.stdout.txt"
    $stderr = Join-Path $outDir "fc1-preload.stderr.txt"
    Set-Content -LiteralPath $stdout -Value '' -NoNewline
    Set-Content -LiteralPath $stderr -Value '' -NoNewline
    $preload = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 300
    Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") "start fresh game process" | Out-Null
    if (-not $preload.WaitForExit(45000)) {
        Stop-Process -Id $preload.Id -Force
        throw "FC-1 preload injector timed out"
    }
    $preloadText = ((Get-Content -Raw -LiteralPath $stdout) + "`n" +
                    (Get-Content -Raw -LiteralPath $stderr))
    $preloadText | Write-Host
    if ($preload.ExitCode -ne 0 -or
        $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "FC-1 preload bootstrap did not reach armed stage"
    }
    Assert-NoStaleFc1Waiter

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
    if (-not $mapped) { throw "FC-0 payload/bootstrap did not map in the fresh process" }
    Assert-CleanTracer $gamePid
    $startTime = Get-ProcessStartTime $gamePid
    [ordered]@{
        version = 1; device = $Device; pid = $gamePid; start_time = $startTime
        payload_sha256 = $pins[$payload]; bootstrap_sha256 = $pins[$bootstrap]
        controller_sha256 = $pins[$controller]
    } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    Write-Output "FC1_PREPARE_FRESH_PROCESS_PASSED pid=$gamePid start_time=$startTime called=0 attached=0 game_writes=0 TracerPid=0"
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeOneFramePassthroughVptrSwap, "one one-frame pass-through vptr swap"),
    @($AcknowledgeNoActionInputNitroOrPhysicsCorrection, "zero action, input, Nitro and physics correction"),
    @($AcknowledgeShortPtraceStallAndCrashRisk, "the short ptrace stall and crash risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) {
    throw "Prepared-process receipt missing"
}
$prepared = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
$startTime = if ($gamePid -gt 0) { Get-ProcessStartTime $gamePid } else { "" }
if ($prepared.version -ne 1 -or $prepared.device -ne $Device -or
    [int]$prepared.pid -ne $gamePid -or [string]$prepared.start_time -ne $startTime -or
    $prepared.payload_sha256 -ne $pins[$payload] -or
    $prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or
    $prepared.controller_sha256 -ne $pins[$controller]) {
    throw "Prepared process receipt mismatch"
}
Assert-CleanTracer $gamePid
foreach ($pair in @(
    @($remotePayload, $pins[$payload]), @($remoteBootstrap, $pins[$bootstrap]),
    @($remoteController, $pins[$controller])
)) { Assert-RemoteHash $pair[0] $pair[1] }
$maps = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
    throw "Prepared FC-1 modules are not mapped in this process"
}

$baseHex = Get-LibraryBaseHex $gamePid
$base = [Convert]::ToUInt64($baseHex, 16)
$remoteReport = "/data/local/tmp/a9tas_fc1_report_${gamePid}_${startTime}.bin"
$localReport = Join-Path $outDir "a9tas_fc1_report_${gamePid}_${startTime}.bin"
if (Test-Path -LiteralPath $localReport) { throw "Local report already exists" }
$remoteExists = ((& $AdbPath -s $Device shell "su -c 'if [ -e $remoteReport ]; then echo EXISTS; fi'" 2>$null) -join '').Trim()
if ($remoteExists -ne '') { throw "Remote report already exists" }

$lines = & $AdbPath -s $Device shell `
    "su -c '$remoteController $gamePid $baseHex $TimeoutMs $remoteReport $controllerAck'" 2>&1
$code = $LASTEXITCODE
$lines | Out-Host
$text = $lines -join "`n"
if ($code -ne 0 -or $text -notmatch "FC1_DONE success=1") {
    throw "FC-1 one-frame controller failed closed"
}
Invoke-AdbChecked @('-s', $Device, 'pull', $remoteReport, $localReport) "pull FC-1 report" | Out-Null
$validationLines = & $python.Source $reportValidator $localReport --pid $gamePid --base "0x$baseHex"
$validationCode = $LASTEXITCODE
$validationLines | Out-Host
if ($validationCode -ne 0 -or
    ($validationLines -join "`n") -notmatch "FC1_REPORT_VALID passed=1 pid=$gamePid base=0x$baseHex") {
    throw "FC-1 report validation failed"
}
if ((Get-GamePid) -ne $gamePid -or (Get-ProcessStartTime $gamePid) -ne $startTime) {
    throw "Game process changed after FC-1"
}
Assert-CleanTracer $gamePid
Start-Sleep -Milliseconds $PostProbeObservationMs
if ((Get-GamePid) -ne $gamePid -or (Get-ProcessStartTime $gamePid) -ne $startTime) {
    throw "Game process changed during delayed FC-1 survival observation"
}
Assert-CleanTracer $gamePid
Write-Output "FC1_PROBE_PASSED pid=$gamePid start_time=$startTime one_frame=1 game_writes=1 action_calls=0 input=0 nitro=0 physics_correction=0 delayed_observation_ms=$PostProbeObservationMs TracerPid=0"
