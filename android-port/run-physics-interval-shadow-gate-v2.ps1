param(
    [ValidateSet("OfflineValidate", "RecordGate")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$ObservationMs = 300,
    [int]$CompletionWaitMs = 1500,
    [string]$OutputPath = "",
    [switch]$AcknowledgeFiveCallRecordGate
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$observer = Join-Path $root "build\physics-interval-readonly-v1\a9tas_physics_interval_readonly_observer_v1"
$controller = Join-Path $root "build\physics-interval-getter-shadow-controller-v2\a9tas_physics_interval_shadow_controller_v2"
$parser = Join-Path $root "tools\parse_physics_interval_readonly_v1.py"
$receiptParser = Join-Path $root "tools\parse_physics_interval_shadow_receipt_v2.py"
$policy = Join-Path $root "tools\test_physics_interval_shadow_gate_v2_policy.py"
$remoteObserver = "/data/local/tmp/a9tas_physics_interval_readonly_observer_v1"
$remoteController = "/data/local/tmp/a9tas_physics_interval_shadow_controller_v2"
$ack = "I_ACCEPT_PHYSICS_INTERVAL_SHADOW_V2"
$limit = 5
$expectedObserverHash = "e8e75ac176650ba48962774a8ad6a209d00f27592d09c935e739722c785a8005"
$expectedControllerHash = "1e1aa77a35fdfd596cc572f98fc99090bd66cc188bdb15b155705e58feee56a0"
$expectedPayloadHash = "f49b78eff51b606436f291c837ab50c297286ae3f43a10352dc2aa77ca8a62df"

foreach ($path in @($observer, $controller, $parser, $receiptParser, $policy)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Gate input: $path"
    }
}
python -B $policy $PSCommandPath
if ($LASTEXITCODE -ne 0) { throw "Gate policy validation failed" }
$observerHash = (Get-FileHash -LiteralPath $observer -Algorithm SHA256).Hash.ToLower()
$controllerHash = (Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLower()
if ($observerHash -ne $expectedObserverHash -or
    $controllerHash -ne $expectedControllerHash) {
    throw "Local Gate artifact hash mismatch"
}
if ($Mode -eq "OfflineValidate") {
    Write-Output "PHYSICS_INTERVAL_SHADOW_GATE_V2_OFFLINE_VALID record_only=1 limit=5 auto_esc_max=1 live_run=0 game_writes=0"
    exit 0
}
if (-not $AcknowledgeFiveCallRecordGate) {
    throw "RecordGate requires -AcknowledgeFiveCallRecordGate"
}
if ($ObservationMs -lt 100 -or $ObservationMs -gt 1000) {
    throw "ObservationMs must be in [100,1000]"
}
if ($CompletionWaitMs -lt 250 -or $CompletionWaitMs -gt 3000) {
    throw "CompletionWaitMs must be in [250,3000]"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

function Invoke-Adb([string[]]$Arguments) {
    $result = & $AdbPath -s $Device @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ADB failed: $($Arguments -join ' ')`n$($result -join "`n")"
    }
    return @($result)
}

function Read-StartTicks([int]$TargetPid) {
    $stat = ((Invoke-Adb @("shell", "su -c 'cat /proc/$TargetPid/stat'")) -join "").Trim()
    $rightParen = $stat.LastIndexOf(')')
    if ($rightParen -lt 1 -or $rightParen + 2 -ge $stat.Length) {
        throw "Malformed /proc/$TargetPid/stat"
    }
    $fields = @($stat.Substring($rightParen + 2) -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Unable to read PID start ticks"
    }
    return [UInt64]$fields[19]
}

function Read-GameIdentity {
    $pidText = ((Invoke-Adb @("shell", "su -c 'pidof $package'")) -join "").Trim()
    if ($pidText -notmatch '^\d+$') { throw "Game PID unavailable or ambiguous: '$pidText'" }
    $targetPid = [int]$pidText
    $maps = Invoke-Adb @("shell", "su -c 'cat /proc/$targetPid/maps'")
    $bases = foreach ($line in $maps) {
        if ($line -notmatch 'libAsphalt9\.so') { continue }
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
            if ([Convert]::ToUInt64($matches[2], 16) -eq 0) {
                [Convert]::ToUInt64($matches[1], 16)
            }
        }
    }
    $bases = @($bases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base, found $($bases.Count)" }
    if (($maps -join "`n") -notmatch '/data/local/tmp/liba9tas_physics_interval_getter_v2_passive\.so') {
        throw "Pinned passive payload is not preloaded"
    }
    return [PSCustomObject]@{
        Pid = $targetPid
        StartTicks = Read-StartTicks $targetPid
        Base = [UInt64]$bases[0]
    }
}

function Assert-RemoteArtifacts {
    $paths = @(
        $remoteObserver,
        $remoteController,
        "/data/local/tmp/liba9tas_physics_interval_getter_v2_passive.so"
    )
    $expected = @(
        $expectedObserverHash,
        $expectedControllerHash,
        $expectedPayloadHash
    )
    for ($index = 0; $index -lt $paths.Count; ++$index) {
        $line = ((Invoke-Adb @("shell", "sha256sum $($paths[$index])")) -join "").Trim()
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
            $matches[1].ToLower() -ne $expected[$index]) {
            throw "Remote artifact hash mismatch: $($paths[$index])"
        }
    }
}

function Assert-RemoteControllerEntrypoint([string]$ProbeOutput) {
    # A deliberately impossible PID must pass the complete CLI parser and
    # fail at the first runtime identity check. This catches argument-layout
    # regressions before the live game object is opened or modified.
    $command = "$remoteController status 2147483646 1 1000 1000 " +
               "record 5 - $ProbeOutput $ack"
    $result = & $AdbPath -s $Device shell "su -c '$command'" 2>&1
    $exitCode = $LASTEXITCODE
    $joined = ($result -join "`n")
    if ($exitCode -ne 3 -or
        $joined -notmatch 'stage=process_identity') {
        throw "Remote controller entrypoint preflight failed: exit=$exitCode output=$joined"
    }
}

function Invoke-Transaction(
    [string]$Action, [object]$Identity, [UInt64]$StepOptions,
    [string]$RemoteOutput) {
    $baseHex = $Identity.Base.ToString("x")
    $objectHex = $StepOptions.ToString("x")
    $command = "$remoteController $Action $($Identity.Pid) $($Identity.StartTicks) $baseHex $objectHex record $limit - $RemoteOutput $ack"
    return Invoke-Adb @("shell", "su -c '$command'")
}

$identity = $null
$stepOptions = [UInt64]0
$installed = $false
$finalized = $false
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$remotePrefix = "/data/local/tmp/a9tas_pig2_$stamp"
$localObservation = Join-Path $root "evidence\a9tas_pig2_locator_$stamp.a9pio2"
try {
    $deviceLine = & $AdbPath devices | Where-Object { $_ -match "^$([regex]::Escape($Device))\s+device$" }
    if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }
    Assert-RemoteArtifacts
    Assert-RemoteControllerEntrypoint "$remotePrefix.entrypoint.a9pgtr2"
    $identity = Read-GameIdentity
    $baseHex = $identity.Base.ToString("x")
    $remoteObservation = "$remotePrefix.a9pio2"
    Invoke-Adb @("shell", "su -c '$remoteObserver $($identity.Pid) $baseHex $ObservationMs 10 $remoteObservation'") | Out-Host
    Invoke-Adb @("pull", $remoteObservation, $localObservation) | Out-Host
    $observationJson = (& python -B $parser $localObservation --json --profile car-physics) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Exact CarPhysics read-only locator failed" }
    $observation = $observationJson | ConvertFrom-Json
    if (-not $observation.passed -or $observation.step_options.Count -ne 1) {
        throw "CarPhysics object identity was not unique and stable"
    }
    $stepOptions = [Convert]::ToUInt64(
        ([string]$observation.step_options[0]).Substring(2), 16)
    $repinned = Read-GameIdentity
    if ($repinned.Pid -ne $identity.Pid -or
        $repinned.StartTicks -ne $identity.StartTicks -or
        $repinned.Base -ne $identity.Base) {
        throw "Game identity changed after read-only location"
    }

    $preflightOutput = "$remotePrefix.preflight.a9pgtr2"
    $preflight = (Invoke-Transaction "preflight" $identity $stepOptions `
                  $preflightOutput) -join "`n"
    $preflight | Write-Output
    if ($preflight -notmatch 'PHYSICS_INTERVAL_SHADOW_PREFLIGHT' -or
        $preflight -notmatch 'game_writes=0') {
        throw "Controller read-only preflight failed"
    }

    $installOutput = "$remotePrefix.install.a9pgtr2"
    $installed = $true
    Invoke-Transaction "install" $identity $stepOptions $installOutput | Out-Host

    # The one and only automatic input in this Gate.
    Invoke-Adb @("shell", "input keyevent 111") | Out-Null
    Start-Sleep -Milliseconds $CompletionWaitMs

    $samePid = ((Invoke-Adb @("shell", "su -c 'pidof $package'")) -join "").Trim()
    if ($samePid -ne [string]$identity.Pid) { throw "Game process changed after ESC" }
    $statusOutput = "$remotePrefix.status.a9pgtr2"
    $status = (Invoke-Transaction "status" $identity $stepOptions $statusOutput) -join "`n"
    $status | Write-Output
    if ($status -notmatch 'PHYSICS_INTERVAL_SHADOW_STATUS complete=1\b') {
        throw "Five-call record did not complete inside the single wait budget"
    }

    $finalOutput = "$remotePrefix.final.a9pgtr2"
    Invoke-Transaction "finalize" $identity $stepOptions $finalOutput | Out-Host
    $finalized = $true
    if ($OutputPath -eq "") {
        $OutputPath = Join-Path $root "evidence\a9tas_physics_interval_shadow_$stamp.a9pgtr2"
    }
    $OutputPath = [IO.Path]::GetFullPath($OutputPath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
    Invoke-Adb @("pull", $finalOutput, $OutputPath) | Out-Host
    $intervalPath = "$OutputPath.intervals.bin"
    $jsonPath = "$OutputPath.analysis.json"
    & python -B $receiptParser $OutputPath --json $jsonPath --extract-intervals $intervalPath
    if ($LASTEXITCODE -ne 0) { throw "Final A9PGTR2 semantic validation failed" }
    $analysis = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    if (-not $analysis.complete -or $analysis.evidence.semantic_errors -ne 0 -or
        $analysis.evidence.object_mismatches -ne 0 -or
        $analysis.evidence.record_calls -ne $limit -or
        $analysis.evidence.overrides -ne 0 -or
        @($analysis.intervals | Where-Object { -not $_.valid }).Count -ne 0 -or
        @($analysis.events | Where-Object {
            $_.flags -ne "0x33" -or $_.requested_bits -ne "0x00000000" -or
            $_.original_bits -ne $_.final_bits -or
            $_.commit_sequence -ne ($_.sequence + 1)
        }).Count -ne 0) {
        throw "Final record evidence failed strict semantic checks"
    }
    $tracer = ((Invoke-Adb @("shell", "grep '^TracerPid:' /proc/$($identity.Pid)/status")) -join "").Trim()
    if ($tracer -notmatch '^TracerPid:\s*0$') { throw "Unexpected tracer state: $tracer" }
    $hash = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash.ToLower()
    Write-Output "PHYSICS_INTERVAL_SHADOW_GATE_V2_PASSED receipt=$OutputPath sha256=$hash calls=5 overrides=0 TracerPid=0"
} finally {
    if ($installed -and -not $finalized -and $null -ne $identity -and $stepOptions -ne 0) {
        try {
            $rollbackOutput = "$remotePrefix.rollback.a9pgtr2"
            Invoke-Transaction "rollback" $identity $stepOptions $rollbackOutput | Out-Host
            Write-Warning "Gate failed; one bounded rollback completed"
        } catch {
            Write-Warning "Gate failed and bounded rollback could not be confirmed: $($_.Exception.Message)"
        }
    }
}
