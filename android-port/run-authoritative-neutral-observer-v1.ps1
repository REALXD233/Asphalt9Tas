# GUARDED five-frame write-neutral observer runner. This file is not runtime
# authorization. Its default useful mode is -OfflineValidateOnly.

param(
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$OfflineValidateOnly,
    [switch]$ExecuteExactlyOneLiveAttempt,
    [switch]$AcknowledgeReviewedBuildOnlyCandidate,
    [switch]$AcknowledgeNaturallyRunningRaceNoManualInput,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeFiveFramePhaseBindingOnly,
    [switch]$AcknowledgeZeroGameplayWritesAndActions,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root `
    "build\authoritative-neutral-observer-v1\a9tas_hwbp_authoritative_neutral_observer_v1"
$source = Join-Path $root "src\hwbp_authoritative_neutral_observer_v1.cpp"
$semanticTest = Join-Path $root "tools\test_authoritative_neutral_observer_v1.py"
$policyTest = Join-Path $root "tools\test_authoritative_neutral_observer_cpp_policy_v1.py"
$validator = Join-Path $root "tools\validate_authoritative_neutral_observer_v1.py"
$readelf = Join-Path $root `
    "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
$objdump = Join-Path $root `
    "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe"
$reviewObject = Join-Path $root `
    "build\authoritative-neutral-observer-v1\hwbp_authoritative_neutral_observer_v1.o"
$pins = @{
    $binary = "98f977d04fe2c399cc544fa5b0dac96c5fd92932ea476da8dd80653e116410e5"
    $source = "3ad4aac949fa9fdc74ec517e7fb5bdd3f843d5a0d460e6a9a5fcf0fa9242058d"
    $semanticTest = "9086fa347bac0326cf145ffa2abe83513c31e85e2aaef1c42fbc38cdfaed981a"
    $policyTest = "df380287d978c0defbd33e976fe8d01ac04b0bcd97a38eaa88867c3e36f04d9b"
    $validator = "70768f6d620d78c0985b00358d496ee6cd6b7f661ef3310e8b4e2f5b78b0ff9d"
}

function Get-LocalSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return -join ($algorithm.ComputeHash($stream) | ForEach-Object {
                $_.ToString("x2")
            })
        } finally { $algorithm.Dispose() }
    } finally { $stream.Dispose() }
}

foreach ($artifact in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing reviewed artifact: $artifact"
    }
    if ((Get-LocalSha256 $artifact) -ne $pins[$artifact]) {
        throw "Reviewed artifact hash mismatch: $artifact"
    }
}
foreach ($tool in @($readelf, $objdump, $reviewObject)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Missing offline review dependency: $tool"
    }
}
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 300000) {
    throw "TimeoutMs must be 1000..300000"
}
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') {
        throw "Invalid explicit address '$value'"
    }
}

Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_authoritative_neutral_observer_v1
    $semanticCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($semanticCode -ne 0) { throw "Neutral observer semantic tests failed" }
python -B $policyTest $binary $reviewObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Neutral observer artifact policy failed" }
python -B $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "A9ANO1 validator selftest failed" }

if ($OfflineValidateOnly) {
    Write-Host "AUTHORITATIVE_NEUTRAL_OBSERVER_OFFLINE_VALIDATION_OK"
    Write-Host "frames=5 target_mem=readonly gameplay_writes=0 action_calls=0 deployed=0"
    Write-Host "candidate_sha256=$($pins[$binary])"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneLiveAttempt, "exactly one live attempt"),
    @($AcknowledgeReviewedBuildOnlyCandidate, "the reviewed BUILD_ONLY candidate"),
    @($AcknowledgeNaturallyRunningRaceNoManualInput, "a naturally running race with no manual input"),
    @($AcknowledgeNoPausedAttach, "that attach while paused is forbidden"),
    @($AcknowledgeFiveFramePhaseBindingOnly, "that this proves phase binding only, not input transport"),
    @($AcknowledgeZeroGameplayWritesAndActions, "zero gameplay writes and action calls"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

$deviceLine = & $AdbPath devices | Where-Object {
    $_ -match "^$([regex]::Escape($Device))\s+device$"
}
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }

$package = "com.aligames.kuang.kybc.aligames"
$ack = "I_ACCEPT_WRITE_NEUTRAL_FIVE_FRAME_OBSERVER_V1"
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') {
    throw "Game process is not running or pidof was ambiguous: '$pidText'"
}
$gamePid = [int]$pidText

function Get-TracerPidLine {
    return (& $AdbPath -s $Device shell `
        "su -c 'grep ^TracerPid: /proc/$gamePid/status'").Trim()
}

$tracerBefore = Get-TracerPidLine
if ($tracerBefore -ne "TracerPid:`t0" -and $tracerBefore -ne "TracerPid: 0") {
    throw "Game already has a tracer: '$tracerBefore'"
}
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) { throw "Failed to read game maps" }
$bases = foreach ($line in $maps) {
    if ($line -match 'libAsphalt9\.so' -and
        $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
        [Convert]::ToUInt64($matches[2], 16) -eq 0) {
        [Convert]::ToUInt64($matches[1], 16)
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) {
    throw "Expected one zero-offset libAsphalt9 mapping, found $($bases.Count)"
}
$baseHex = $bases[0].ToString("x")

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($OutputPath -eq "") {
    $OutputPath = Join-Path $root `
        "evidence\a9tas_authoritative_neutral_$stamp.a9ano1"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) {
    throw "Local report path already exists: $OutputPath"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$jsonOutput = "$OutputPath.json"
if (Test-Path -LiteralPath $jsonOutput) {
    throw "Local JSON output already exists: $jsonOutput"
}
$remoteBinary = "/data/local/tmp/a9tas_hwbp_authoritative_neutral_observer_v1"
$remoteReport = "/data/local/tmp/a9tas_authoritative_neutral_${gamePid}_$stamp.a9ano1"

function Get-RemoteSha256([string]$RemotePath) {
    $hashLine = ((& $AdbPath -s $Device shell `
        "su -c 'sha256sum $RemotePath'") | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "Could not verify remote SHA256 for $RemotePath"
    }
    return $matches[1].ToLowerInvariant()
}

function Assert-StableGameAndDetach {
    $pidAfter = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
    if ($LASTEXITCODE -ne 0 -or $pidAfter -ne $pidText) {
        throw "Game process exited or changed PID during neutral observation"
    }
    $tracerAfter = Get-TracerPidLine
    if ($tracerAfter -ne "TracerPid:`t0" -and $tracerAfter -ne "TracerPid: 0") {
        throw "Neutral observer left a tracer attached: '$tracerAfter'"
    }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Neutral observer push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Neutral observer chmod failed" }
if ((Get-RemoteSha256 $remoteBinary) -ne $pins[$binary]) {
    throw "Remote neutral observer hash differs after push"
}

Write-Host "AUTHORITATIVE_NEUTRAL_OBSERVER_START pid=$gamePid frames=5 target_mem=readonly"
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteReport $ack $PhysicsContextHex $MainObjectHex $FinalOwnerHex'" |
    Out-Host
$runCode = $LASTEXITCODE
Assert-StableGameAndDetach
if ($runCode -ne 0) {
    throw "Neutral observer failed closed (exit=$runCode); game survived and TracerPid=0"
}

& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Neutral report chmod failed" }
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Neutral report pull failed" }
python -B $validator $OutputPath --json-out $jsonOutput
if ($LASTEXITCODE -ne 0) { throw "A9ANO1 report validation failed" }
Assert-StableGameAndDetach
Write-Host "AUTHORITATIVE_NEUTRAL_OBSERVER_LIVE_PASSED report=$OutputPath json=$jsonOutput TracerPid=0"
