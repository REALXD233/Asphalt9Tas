param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ProbePreparedProcess")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 8000,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgeGettidOnlyModulesLoadedButNotCalled,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeSt2SignalCatcherRevalidation,
    [switch]$AcknowledgeOneProducerGettidOnly,
    [switch]$AcknowledgeZeroGameCallsActionsAndGameplayWrites,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"

# LIVE ROUTE RETIRED 2026-08-18:
# A gettid-only NativeBridge call on the natural Houdini producer returned the
# expected TID and passed host-register/stack rollback, but the same guest
# thread then crashed with ARM64 pc=0.  Host-level rollback does not cover the
# hidden Houdini guest context.  Keep the offline validator for auditability,
# but fail before artifact validation, ADB access, restart, attach, or calls.
if ($Mode -ne "OfflineValidateOnly") {
    throw "PT-NB0 live modes are permanently retired after the 2026-08-18 guest-context crash; offline validation only"
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\producer-thread-nativebridge-probe-v1"
$payload = Join-Path $outDir "liba9tas_producer_thread_probe_v1.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_producer_thread_probe_v1.so"
$controller = Join-Path $outDir "a9tas_producer_thread_nativebridge_probe_controller_v1"
$st2Controller = Join-Path $root "build\same-thread-probe-v1\a9tas_same_thread_probe_controller_v1"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_producer_thread_probe_preload_v1.sh"
$controllerSource = Join-Path $root "src\producer_thread_nativebridge_probe_controller_v1.cpp"
$payloadSource = Join-Path $root "src\payload_same_thread_probe_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_producer_thread_probe_v1_build.cpp"
$buildScript = Join-Path $root "build-producer-thread-nativebridge-probe-v1.ps1"
$policyTest = Join-Path $root "tools\test_producer_thread_nativebridge_probe_policy_v1.py"
$runnerPolicyTest = Join-Path $root "tools\test_run_producer_thread_probe_policy_v1.py"

$pins = @{
    $payload = "f68c3396695ec715428a2cb4508b2943f1975be745b5e9d67e78e34ef3d00082"
    $bootstrap = "4c544d16bf97ce72692a526484088f6088205e4fb8e4881ab216a0ff0574ccdf"
    $controller = "558dfbe94cdec941a645a73735ff06e78c81d8e3c5a504af5b57636912118916"
    $st2Controller = "a689b498bb369b628dc379927e20cee1571fab6a01d4ef419d2cdf34f5f9382e"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "bbbf65b2b90f01dc7186dab1dbfa467deb2c3ed3944f41f4cfb848f858a6995c"
    $controllerSource = "30c876d40bee258abf57f7c8f40f9e1ced80786b940c4575e151f98aa45b172b"
    $payloadSource = "a63c565041fe7cfe5b6c1bc00978ea6b93807070025f1c1dfc0fc4f5084e4c02"
    $bootstrapSource = "516538ebc67bd1b5ba6e8688cbcc69b472ee6b5de518182d5794adc601854b25"
    $buildScript = "c24763e18518f0809baded95e986f485230f0ee040b0a4f37236829f22c4cd71"
    $policyTest = "2eb49ea717cd8a83aec0335d14b26e99ff203bc4b12441a798921f42b5880e27"
}

$remotePayload = "/data/local/tmp/liba9tas_producer_thread_probe_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_producer_thread_probe_v1.so"
$remoteController = "/data/local/tmp/a9tas_producer_thread_nativebridge_probe_controller_v1"
$remoteSt2Controller = "/data/local/tmp/a9tas_same_thread_probe_controller_producer_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_producer_thread_v1"
$remoteHelper = "/data/local/tmp/run_producer_thread_probe_preload_v1.sh"
$probeAck = "I_ACCEPT_PRODUCER_THREAD_NATIVEBRIDGE_GETTID_PROBE_V1"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Missing reviewed producer-thread artifact: $artifact"
        }
        if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Reviewed producer-thread artifact hash mismatch: $artifact"
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
& $python.Source $policyTest
if ($LASTEXITCODE -ne 0) { throw "PT-NB0 controller policy tests failed" }
& $python.Source $runnerPolicyTest
if ($LASTEXITCODE -ne 0) { throw "PT-NB0 runner policy tests failed" }
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 15000) {
    throw "TimeoutMs must be 1000..15000"
}

if ($Mode -eq "OfflineValidateOnly") {
    $sampleSt2 = "SAME_THREAD_PROBE_V1_RESULT passed=1 pid=4607 tid=4613 status=1 trampoline=0x7ffff4533560 returned_tid=4613 alive=1 tracer_pid=0"
    if ($sampleSt2 -notmatch 'SAME_THREAD_PROBE_V1_RESULT passed=1 pid=4607 tid=(\d+) status=1 trampoline=0x([0-9a-fA-F]+) returned_tid=\1 alive=1 tracer_pid=0') {
        throw "Offline ST-2 parser selftest failed"
    }
    $sampleProbe = "PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_RESULT passed=1 pid=4607 producer_tid=4711 producer_name=Thread-905 producer_hits=2 guest_tid=4711 delta_before=16666 delta_after=16666 transport_attempted=1 call_result=success rollback=1 detached=1 unexpected_stops=0 alive=1 tracer_pid=0 host_calls=0 game_calls=0 gameplay_writes=0"
    $probeMatch = [regex]::Match(
        $sampleProbe,
        'PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_RESULT passed=1 pid=4607 producer_tid=(\d+) producer_name=(\S+) producer_hits=2 guest_tid=(\d+) delta_before=(\d+) delta_after=(\d+) transport_attempted=1 call_result=success rollback=1 detached=1 unexpected_stops=0 alive=1 tracer_pid=0 host_calls=0 game_calls=0 gameplay_writes=0')
    if (-not $probeMatch.Success -or
        $probeMatch.Groups[1].Value -ne $probeMatch.Groups[3].Value -or
        $probeMatch.Groups[4].Value -ne $probeMatch.Groups[5].Value) {
        throw "Offline PT-NB0 result parser selftest failed"
    }
    Write-Output "PT_NB0_RUNNER_OFFLINE_VALIDATION_OK device_access=0 restart=0 attached=0 guest_calls=0 game_calls=0 gameplay_writes=0"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

if ($Mode -eq "PrepareFreshProcess") {
    foreach ($gate in @(
        @($AcknowledgeFreshGameProcessRestart, "fresh game process restart"),
        @($AcknowledgePreloadWindowInjection, "preload-window injection"),
        @($AcknowledgeGettidOnlyModulesLoadedButNotCalled,
          "that gettid-only modules are loaded but not called")
    )) {
        if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
    }

    foreach ($pair in @(
        @($payload, $remotePayload),
        @($bootstrap, $remoteBootstrap),
        @($controller, $remoteController),
        @($st2Controller, $remoteSt2Controller),
        @($injector, $remoteInjector),
        @($helper, $remoteHelper)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) `
            "push $($pair[1])" | Out-Null
    }
    Invoke-AdbChecked @(
        '-s', $Device, 'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteSt2Controller $remoteInjector $remoteHelper'"
    ) "device chmod" | Out-Null
    foreach ($remotePin in @(
        @($remotePayload, $pins[$payload]),
        @($remoteBootstrap, $pins[$bootstrap]),
        @($remoteController, $pins[$controller]),
        @($remoteSt2Controller, $pins[$st2Controller]),
        @($remoteInjector, $pins[$injector]),
        @($remoteHelper, $pins[$helper])
    )) {
        Assert-RemoteHash $remotePin[0] $remotePin[1]
    }

    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") `
        "force-stop old game process" | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }

    $stdout = Join-Path $outDir "producer_preload.stdout.txt"
    $stderr = Join-Path $outDir "producer_preload.stderr.txt"
    Set-Content -LiteralPath $stdout -Value '' -NoNewline
    Set-Content -LiteralPath $stderr -Value '' -NoNewline
    $injectProcess = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 300
    Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") `
        "start fresh game process" | Out-Null
    if (-not $injectProcess.WaitForExit(45000)) {
        Stop-Process -Id $injectProcess.Id -Force
        throw "Preload injector timed out"
    }
    $injectText = ((Get-Content -Raw -LiteralPath $stdout) + "`n" +
                   (Get-Content -Raw -LiteralPath $stderr))
    $injectText | Write-Host
    if ($injectProcess.ExitCode -ne 0 -or
        $injectText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "Producer-thread preload bootstrap did not pass armed stage"
    }

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
    if (-not $mapped) { throw "PT-NB0 isolated payload/bootstrap did not map" }
    Assert-CleanTracer $gamePid
    Write-Output "PT_NB0_PREPARE_FRESH_PROCESS_PASSED pid=$gamePid called=0 attached=0 TracerPid=0"
    Write-Output "Enter a naturally running race, then run ProbePreparedProcess with all probe acknowledgements."
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeSt2SignalCatcherRevalidation,
      "one Signal Catcher ST-2 revalidation"),
    @($AcknowledgeOneProducerGettidOnly,
      "one producer-thread gettid-only call"),
    @($AcknowledgeZeroGameCallsActionsAndGameplayWrites,
      "zero game calls, actions, and gameplay writes"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}

$gamePid = Get-GamePid
if ($gamePid -le 0) { throw "Game PID unavailable" }
Assert-CleanTracer $gamePid
foreach ($remotePin in @(
    @($remotePayload, $pins[$payload]),
    @($remoteBootstrap, $pins[$bootstrap]),
    @($remoteController, $pins[$controller]),
    @($remoteSt2Controller, $pins[$st2Controller])
)) {
    Assert-RemoteHash $remotePin[0] $remotePin[1]
}
$mapsBefore = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
if (-not $mapsBefore.Contains($remotePayload) -or
    -not $mapsBefore.Contains($remoteBootstrap)) {
    throw "Prepared PT-NB0 modules are not mapped in this process"
}

$selftestLines = & $AdbPath -s $Device shell "su -c '$remoteController --selftest'" 2>&1
$selftestCode = $LASTEXITCODE
$selftestLines | Out-Host
if ($selftestCode -ne 0 -or
    ($selftestLines -join "`n") -notmatch 'PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_SELFTEST passed=1 dr7=0x90001 required_hits=2 guest_calls=0 gameplay_writes=0') {
    throw "PT-NB0 controller device selftest failed"
}

$st2Lines = & $AdbPath -s $Device shell "su -c '$remoteSt2Controller $gamePid $remoteBootstrap $remotePayload'" 2>&1
$st2Code = $LASTEXITCODE
$st2Lines | Out-Host
$st2Text = $st2Lines -join "`n"
if ($st2Code -ne 0 -or
    $st2Text -notmatch "SAME_THREAD_PROBE_V1_RESULT passed=1 pid=$gamePid tid=(\d+) status=1 trampoline=0x([0-9a-fA-F]+) returned_tid=\1 alive=1 tracer_pid=0") {
    throw "Same-process ST-2 revalidation failed"
}
$trampolineHex = $matches[2].ToLowerInvariant()
if ((Get-GamePid) -ne $gamePid) { throw "Process changed after ST-2" }
Assert-CleanTracer $gamePid
$baseHex = Get-LibraryBaseHex $gamePid

$probeLines = & $AdbPath -s $Device shell `
    "su -c '$remoteController $gamePid $baseHex 0 $trampolineHex $TimeoutMs 0 $probeAck'" 2>&1
$probeCode = $LASTEXITCODE
$probeLines | Out-Host
$probeText = $probeLines -join "`n"
$probePattern = "PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_RESULT passed=1 pid=$gamePid producer_tid=(\d+) producer_name=(\S+) producer_hits=2 guest_tid=(\d+) delta_before=(\d+) delta_after=(\d+) transport_attempted=1 call_result=success rollback=1 detached=1 unexpected_stops=0 alive=1 tracer_pid=0 host_calls=0 game_calls=0 gameplay_writes=0"
$probeMatch = [regex]::Match($probeText, $probePattern)
if ($probeCode -ne 0 -or -not $probeMatch.Success -or
    $probeMatch.Groups[1].Value -ne $probeMatch.Groups[3].Value -or
    $probeMatch.Groups[4].Value -ne $probeMatch.Groups[5].Value) {
    throw "PT-NB0 producer-thread transport probe failed closed"
}
if ((Get-GamePid) -ne $gamePid) { throw "Game process changed after PT-NB0" }
Assert-CleanTracer $gamePid
Write-Output "PT_NB0_PROBE_PASSED pid=$gamePid producer_tid=$($probeMatch.Groups[1].Value) producer_name=$($probeMatch.Groups[2].Value) trampoline=0x$trampolineHex guest_calls=1 game_calls=0 gameplay_writes=0 TracerPid=0"
