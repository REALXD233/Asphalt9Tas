# GUARDED Gate 11 observe-only runner. This file is not authorization and does
# not deploy/restart the guest payload.
param(
    [int]$TimeoutMs = 10000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ReportOutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$PayloadStage2ObserveValidated,
    [switch]$AcknowledgeNaturallyRunningRaceNoManualInput,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeOneFixedDeltaWrite,
    [switch]$AcknowledgeExactlyOneObserveRequestZeroActivations,
    [switch]$AcknowledgeAllControlsActionsAndPhysicsCorrectionSkipped,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\staging\a9tas_hwbp_unified_nitro_observe_gate_v1"
$payload = Join-Path $root "build\staging\liba9tas_payload_nitro_rpc_v1.so"
$source = Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$client = Join-Path $root "src\unified_nitro_rpc_client_v1.h"
$protocol = Join-Path $root "src\nitro_rpc_protocol_v1.h"
$input = Join-Path $root "evidence\a9tas_gate6_phase_only_1f_draft_20260817.a9utk1"
$parser = Join-Path $root "tools\parse_unified_nitro_observe_report_v7.py"
$test = Join-Path $root "tools\test_unified_nitro_observe_gate_v1.py"
$pins = @{
    $binary = "1682d264bf424c5c69c5f0b191c6f0d5d9d7f4d54631f022b874379a9ed59d14"
    $payload = "1e65cb02b5eb3a1942b6aee216f4d514f8f19c2306e0f8968900a9e84d9732ca"
    $source = "d25a4607bdf66b444ca5676e2a0e0e071a8cac5baea2272bf4f85c9a23f90b10"
    $client = "5a6f566f15b73c976f325fba89c62905752a00d8372ad4ac120d442f5047fa4b"
    $protocol = "13626fe4901b572884d8ea6b32ba3bf1ccc4bca993231fd66b82bb446eac469d"
    $input = "cec719af384749cf6e9bdcec7f31d01334917a59756ba93c19a04d9ef20af8b8"
    $parser = "6ae5307de48343d2c9a59954b3029a18623c646aff1ff254ad1a766d90f48bba"
    $test = "3370df67df7a1bdbe46a94ccc107a5a1bb1c4e774b06aa49924d83700c3560a6"
}

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return -join ($algorithm.ComputeHash($stream) | ForEach-Object {
                $_.ToString("x2")
            })
        } finally {
            $algorithm.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

foreach ($artifact in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing reviewed artifact: $artifact"
    }
    if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
        throw "Reviewed artifact hash mismatch: $artifact"
    }
}

python -B (Join-Path $root "tools\unified_tick_recording_v1.py") $input
if ($LASTEXITCODE -ne 0) { throw "Gate 11 A9UTK1 input invalid" }
Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_unified_nitro_observe_gate_v1
    $testCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($testCode -ne 0) { throw "Gate 11 offline tests failed" }
if ($OfflineValidateOnly) {
    Write-Host "GATE11_NITRO_OBSERVE_OFFLINE_VALIDATION_OK"
    return
}

foreach ($gate in @(
    @($PayloadStage2ObserveValidated, "the separate Stage 2 observe-only handshake"),
    @($AcknowledgeNaturallyRunningRaceNoManualInput, "a naturally running race with no manual input"),
    @($AcknowledgeNoPausedAttach, "that attach while paused is forbidden"),
    @($AcknowledgeOneFixedDeltaWrite, "one fixed-delta write"),
    @($AcknowledgeExactlyOneObserveRequestZeroActivations, "exactly one RPC request with activations=0"),
    @($AcknowledgeAllControlsActionsAndPhysicsCorrectionSkipped, "all controls/actions/physics correction skipped"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 30000) {
    throw "TimeoutMs must be 1000..30000"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found"
}

$package = "com.aligames.kuang.kybc.aligames"
$ack = "I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1"
$remotePayload = "/data/local/tmp/liba9tas_payload.so"
$socket = "/data/user/0/$package/files/a9tas-nitro-rpc-v1.sock"
$status = "/data/user/0/$package/files/a9tas-nitro-rpc-v1.status"
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($pidText -notmatch '^\d+$') { throw "Game PID unavailable" }
$gamePid = [int]$pidText
$tracer = (& $AdbPath -s $Device shell "grep '^TracerPid:' /proc/$gamePid/status").Trim()
if ($tracer -ne "TracerPid:`t0" -and $tracer -ne "TracerPid: 0") {
    throw "Game already traced: $tracer"
}
$payloadHashLine = (& $AdbPath -s $Device shell "su -c 'sha256sum $remotePayload'").Trim()
if ($payloadHashLine -notmatch '^([0-9a-fA-F]{64})\s+' -or
    $matches[1].ToLowerInvariant() -ne $pins[$payload]) {
    throw "Device Nitro RPC payload hash mismatch"
}
$payloadStatus = & $AdbPath -s $Device shell "su -c 'cat $status'"
if (($payloadStatus -join "`n") -notmatch "(?m)^pid=$gamePid$" -or
    ($payloadStatus -join "`n") -notmatch '(?m)^phase=ready$') {
    throw "Nitro RPC payload status is not ready for this PID"
}
$socketCheck = (& $AdbPath -s $Device shell "su -c 'test -S $socket; echo `$?'").Trim()
if ($socketCheck -ne "0") { throw "Nitro RPC socket is unavailable" }
$payloadMap = & $AdbPath -s $Device shell "su -c 'grep -F $remotePayload /proc/$gamePid/maps'"
if (-not ($payloadMap -join "`n").Contains($remotePayload)) {
    throw "Nitro RPC payload is not mapped in the current game process"
}

$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$bases = foreach ($line in $maps) {
    if ($line -match 'libAsphalt9\.so' -and
        $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
        [Convert]::ToUInt64($matches[2], 16) -eq 0) {
        [Convert]::ToUInt64($matches[1], 16)
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
$baseHex = $bases[0].ToString("x")

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($ReportOutputPath -eq "") {
    $ReportOutputPath = Join-Path $root "evidence\a9tas_gate11_nitro_observe_$stamp.a9uer7"
}
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
if (Test-Path -LiteralPath $ReportOutputPath) { throw "Output already exists" }
$remoteBinary = "/data/local/tmp/a9tas_hwbp_unified_nitro_observe_gate_v1"
$remoteInput = "/data/local/tmp/a9tas_gate11_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_gate11_${gamePid}_$stamp.a9uer7"
foreach ($pair in @(
    [pscustomobject]@{ Local = $binary; Remote = $remoteBinary },
    [pscustomobject]@{ Local = $input; Remote = $remoteInput }
)) {
    & $AdbPath -s $Device push $pair.Local $pair.Remote | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair.Remote)" }
}

& $AdbPath -s $Device shell `
    "su -c 'chmod 700 $remoteBinary; $remoteBinary $gamePid $baseHex $TimeoutMs $remoteInput $remoteReport $ack'" |
    Out-Host
$runCode = $LASTEXITCODE
$afterPid = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
$tracerAfter = (& $AdbPath -s $Device shell "grep '^TracerPid:' /proc/$gamePid/status").Trim()
if ($afterPid -ne $pidText -or
    ($tracerAfter -ne "TracerPid:`t0" -and $tracerAfter -ne "TracerPid: 0")) {
    throw "Gate 11 did not cleanly detach or the game process changed"
}
if ($runCode -ne 0) { throw "Gate 11 failed closed (exit=$runCode)" }
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
& $AdbPath -s $Device pull $remoteReport $ReportOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Gate 11 report pull failed" }
python -B $parser $ReportOutputPath
if ($LASTEXITCODE -ne 0) { throw "Gate 11 A9UER7 verification failed" }
Write-Host "GATE11_NITRO_OBSERVE_PASSED report=$ReportOutputPath activations=0 TracerPid=0"
