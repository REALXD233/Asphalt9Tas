param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ArmExistingProcess")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$CaptureTimeoutMs = 15000,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgeLiveCandidateLoadedButNotArmed,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeOneShotGuestTextPatch,
    [switch]$AcknowledgeSignalCatcherShortStall,
    [switch]$AcknowledgeNoNitroInputMailboxOrPhysicsWrites,
    [switch]$AcknowledgeHookPersistsUntilGameRestart
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Route audit 2026-08-18: Houdini has already shown that patching translated
# guest ARM64 text is not a safe basis for the final TAS.  Keep this runner
# available for reproducible offline validation, but make every live mode
# fail closed before ADB, process restart, injection, ptrace, or patching.
if ($Mode -ne "OfflineValidateOnly") {
    throw "P1 live modes are frozen by the 2026-08-18 route audit. Use run-hwbp-executor-stack-affinity-v1.ps1 for the host-only read-only observer."
}

$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\physics-executor-affinity-v1-live-candidate"
$payload = Join-Path $outDir "liba9tas_physics_executor_affinity_v1_live_candidate.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_physics_executor_affinity_v1_live_candidate.so"
$controller = Join-Path $outDir "a9tas_physics_executor_affinity_controller_v1"
$bootstrapDiag = Join-Path $outDir "a9tas_physics_executor_affinity_bootstrap_diag_v1"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_p1_preload_inject_v1.sh"
$parser = Join-Path $root "tools\parse_physics_executor_affinity_v1.py"
$parserTests = Join-Path $root "tools\test_parse_physics_executor_affinity_v1.py"

$pins = @{
    $payload = "1bde4dde27471645ec30db6085eeada75f5b43afa5bb35dd044c843a3a93b9d3"
    $bootstrap = "459f068c51c86278a71dec6d6ed5f99df79f2e3ef687d3028c624a70c15d866c"
    $controller = "a4cdb2b2709f1155edbead7e8c9fbd5cf43e27a88a956162a41ecd082689790e"
    $bootstrapDiag = "e61a4ce81a3a38e4d5bb945b601892f06e980607e39d2fb690feb830b0727973"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "64480f32b2e1ee3f915b1c6ba209ed867bd4c0b5a2d6b4eea1ad7164d5baac76"
}

$remotePayload = "/data/local/tmp/liba9tas_physics_executor_affinity_v1_live_candidate.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_physics_executor_affinity_v1_live_candidate.so"
$remoteController = "/data/local/tmp/a9tas_physics_executor_affinity_controller_v1"
$remoteBootstrapDiag = "/data/local/tmp/a9tas_physics_executor_affinity_bootstrap_diag_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_p1_v1"
$remoteHelper = "/data/local/tmp/run_p1_preload_inject_v1.sh"
$remoteReport = "/data/local/tmp/a9tas_physics_executor_affinity_p1_report.bin"
$armAck = "I_ACCEPT_P1_EXECUTOR_AFFINITY_LIVE_CANDIDATE_V1"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Missing reviewed P1 artifact: $artifact"
        }
        if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Reviewed P1 artifact hash mismatch: $artifact"
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

Assert-LocalArtifacts
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $parserTests
if ($LASTEXITCODE -ne 0) { throw "P1 report parser selftests failed" }

if ($Mode -eq "OfflineValidateOnly") {
    $source = Get-Content -Raw -LiteralPath `
        (Join-Path $root "src\physics_executor_affinity_controller_v1.cpp")
    foreach ($forbidden in @("WriteRemote(", "Nitro", "nitro", "mailbox")) {
        if ($source.Contains($forbidden)) {
            throw "P1 controller source audit rejected: $forbidden"
        }
    }
    if (([regex]::Matches($source, "RemoteCallSessionCall\(")).Count -ne 6) {
        throw "P1 controller remote-call count is not exactly status/getter/preflight-low/preflight-high/permission-preflight/arm"
    }
    Write-Output "P1_EXECUTOR_AFFINITY_V1_OFFLINE_VALIDATION_OK deployed=0 armed=0 capture_limit=300"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}
if ($CaptureTimeoutMs -lt 5000 -or $CaptureTimeoutMs -gt 30000) {
    throw "CaptureTimeoutMs must be 5000..30000"
}

if ($Mode -eq "PrepareFreshProcess") {
    foreach ($gate in @(
        @($AcknowledgeFreshGameProcessRestart, "fresh game process restart"),
        @($AcknowledgePreloadWindowInjection, "preload-window injection"),
        @($AcknowledgeLiveCandidateLoadedButNotArmed,
          "that the live candidate is loaded but remains unarmed")
    )) {
        if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
    }

    foreach ($pair in @(
        @($payload, $remotePayload),
        @($bootstrap, $remoteBootstrap),
        @($controller, $remoteController),
        @($bootstrapDiag, $remoteBootstrapDiag),
        @($injector, $remoteInjector),
        @($helper, $remoteHelper)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) `
            "push $($pair[1])" | Out-Null
    }
    Invoke-AdbChecked @(
        '-s', $Device, 'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteBootstrapDiag $remoteInjector $remoteHelper'"
    ) "device chmod" | Out-Null
    Assert-RemoteHash $remotePayload $pins[$payload]
    Assert-RemoteHash $remoteBootstrap $pins[$bootstrap]
    Assert-RemoteHash $remoteController $pins[$controller]
    Assert-RemoteHash $remoteBootstrapDiag $pins[$bootstrapDiag]
    Assert-RemoteHash $remoteInjector $pins[$injector]
    Assert-RemoteHash $remoteHelper $pins[$helper]

    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") `
        "force-stop old game process" | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }

    $stdout = Join-Path $outDir "p1_preload_inject.stdout.txt"
    $stderr = Join-Path $outDir "p1_preload_inject.stderr.txt"
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
        throw "Preload bootstrap injection did not pass the armed-stage gate"
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
    if (-not $mapped) { throw "P1 isolated payload/bootstrap did not map" }
    Assert-CleanTracer $gamePid
    $diagLines = & $AdbPath -s $Device shell `
        "su -c '$remoteBootstrapDiag $gamePid $remoteBootstrap $remotePayload'" 2>&1
    $diagCode = $LASTEXITCODE
    $diagLines | Out-Host
    $diagText = $diagLines -join "`n"
    if ($diagCode -ne 0 -or
        $diagText -notmatch "P1_BOOTSTRAP_DIAG_V1 passed=1 pid=$gamePid .*status=1 .*stage=5 .*trampoline=0x[1-9a-fA-F][0-9a-fA-F]* .*guest_calls=0 alive=1 tracer_pid=0") {
        throw "P1 guest payload/trampoline readiness gate did not pass"
    }
    Assert-CleanTracer $gamePid
    Write-Output "P1_PREPARE_FRESH_PROCESS_PASSED pid=$gamePid armed=0"
    Write-Output "Enter a naturally running race, then use Mode=ArmExistingProcess with all arm acknowledgements."
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeOneShotGuestTextPatch, "the one-shot guest text patch"),
    @($AcknowledgeSignalCatcherShortStall, "the short Signal Catcher stall"),
    @($AcknowledgeNoNitroInputMailboxOrPhysicsWrites,
      "zero Nitro, input, mailbox, and physics-state writes"),
    @($AcknowledgeHookPersistsUntilGameRestart,
      "that the observer hook persists until the game process restarts")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}

$gamePid = Get-GamePid
if ($gamePid -le 0) { throw "Game PID unavailable" }
Assert-CleanTracer $gamePid
Assert-RemoteHash $remotePayload $pins[$payload]
Assert-RemoteHash $remoteBootstrap $pins[$bootstrap]
Assert-RemoteHash $remoteController $pins[$controller]
$mapsBefore = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'" 2>$null) -join "`n"
if (-not $mapsBefore.Contains($remotePayload) -or
    -not $mapsBefore.Contains($remoteBootstrap)) {
    throw "Prepared P1 isolated modules are not mapped in this process"
}

$preflightInvocationBegan = $false
$armInvocationBegan = $false
try {
    $controllerLines = & $AdbPath -s $Device shell `
        "su -c '$remoteController $gamePid $remoteBootstrap $remotePayload $remoteReport $CaptureTimeoutMs $armAck'" 2>&1
    $controllerCode = $LASTEXITCODE
    $controllerLines | Out-Host
    $controllerText = $controllerLines -join "`n"
    $preflightInvocationBegan =
        $controllerText -match 'P1_PREFLIGHT_LOW_CALL_BEGIN'
    $armInvocationBegan = $controllerText -match 'P1_ARM_CALL_BEGIN'
    if ($controllerCode -ne 0 -or
        $controllerText -notmatch "PHYSICS_EXECUTOR_AFFINITY_V1_RESULT passed=1 pid=$gamePid .*events=300 .*alive=1 tracer_pid=0") {
        throw "P1 one-shot capture did not pass"
    }

    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
    $localReport = Join-Path $root "evidence\physics_executor_affinity_p1_$stamp.a9pea1"
    Invoke-AdbChecked @('-s', $Device, 'pull', $remoteReport, $localReport) `
        "pull P1 report" | Out-Null
    if ((Get-Item -LiteralPath $localReport).Length -ne 229504) {
        throw "Pulled P1 report size mismatch"
    }
    $parserLines = & $python.Source $parser $localReport --require-installed 2>&1
    $parserCode = $LASTEXITCODE
    $parserLines | Out-Host
    if ($parserCode -ne 0) { throw "P1 report parser rejected the capture" }
    $parserLines | Set-Content -LiteralPath "$localReport.txt"
    if ((Get-GamePid) -ne $gamePid) { throw "Game process changed after capture" }
    Assert-CleanTracer $gamePid
    Write-Output "P1_ARM_CAPTURE_PASSED pid=$gamePid report=$localReport sha256=$(Get-Sha256 $localReport)"
} finally {
    if ($armInvocationBegan) {
        Write-Warning "P1 arm was invoked. Restart the game process before any retry or unrelated live experiment."
    } elseif ($preflightInvocationBegan) {
        Write-Warning "P1 guest address preflight was consumed without an arm marker. No hook was indicated, but restart the game process before retrying."
    }
}
