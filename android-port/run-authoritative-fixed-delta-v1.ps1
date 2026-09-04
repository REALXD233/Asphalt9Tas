# GUARDED five-frame authoritative fixed-delta runner. This file is not
# runtime authorization. Its default useful mode is -OfflineValidateOnly.

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
    [switch]$AcknowledgeExactlyFiveFixedDeltaWrites,
    [switch]$AcknowledgeAllOtherCapabilitiesSkipped,
    [switch]$AcknowledgeFiveFrameBindingNotTrajectoryClaim,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root `
    "build\authoritative-fixed-delta-v1\a9tas_hwbp_authoritative_fixed_delta_v1"
$reviewObject = Join-Path $root `
    "build\authoritative-fixed-delta-v1\hwbp_authoritative_fixed_delta_v1.o"
$source = Join-Path $root "src\hwbp_authoritative_fixed_delta_v1.cpp"
$semanticTest = Join-Path $root "tools\test_authoritative_fixed_delta_v1.py"
$policyTest = Join-Path $root "tools\test_authoritative_fixed_delta_cpp_policy_v1.py"
$validator = Join-Path $root "tools\validate_authoritative_fixed_delta_v1.py"
$readelf = Join-Path $root `
    "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
$objdump = Join-Path $root `
    "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe"
$pins = @{
    $binary = "e3e4306a4e8d23093c4a0a49f17f85b24cdb113bdf549be5ed9d4b06fa6a865c"
    $reviewObject = "c4c15a61fa76bd101b7c08de7a8955d046794051b65e07fc050e0d46cbd53f2c"
    $source = "80ef70ccd63958ab1982ca84ee4559466e365a945f02bcfe29762a6c85411288"
    $semanticTest = "0b0618ef1e7dd890f5e9cbf99460c26e287b5cbb3831b7638cb9d2a289b48323"
    $policyTest = "84deefc103ee600aac16f3abb57754a585c19372df0cf8612569f2b698eee89b"
    $validator = "64b1d7061ab1ab565b22ce2972690e8f62fc3f7ca386f3eff0ef3c47546be858"
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
foreach ($tool in @($readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Missing offline review tool: $tool"
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
    python -B -m unittest test_authoritative_fixed_delta_v1
    $semanticCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($semanticCode -ne 0) { throw "Fixed-delta semantic tests failed" }
python -B $policyTest $binary $reviewObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Fixed-delta artifact policy failed" }
python -B $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "A9AFD1 validator selftest failed" }

if ($OfflineValidateOnly) {
    Write-Host "AUTHORITATIVE_FIXED_DELTA_OFFLINE_VALIDATION_OK"
    Write-Host "frames=5 delta_writes=5 all_other_capabilities=skipped deployed=0"
    Write-Host "candidate_sha256=$($pins[$binary])"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneLiveAttempt, "exactly one live attempt"),
    @($AcknowledgeReviewedBuildOnlyCandidate, "the reviewed BUILD_ONLY candidate"),
    @($AcknowledgeNaturallyRunningRaceNoManualInput, "a naturally running race with no manual input"),
    @($AcknowledgeNoPausedAttach, "that attach while paused is forbidden"),
    @($AcknowledgeExactlyFiveFixedDeltaWrites, "exactly five verified 8-byte fixed-delta writes"),
    @($AcknowledgeAllOtherCapabilitiesSkipped, "all controls, actions and corrections skipped"),
    @($AcknowledgeFiveFrameBindingNotTrajectoryClaim, "that five-frame binding is not a trajectory claim"),
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
$ack = "I_ACCEPT_FIVE_AUTHORITATIVE_FIXED_DELTA_WRITES_V1"
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
        "evidence\a9tas_authoritative_fixed_delta_$stamp.a9afd1"
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
$remoteBinary = "/data/local/tmp/a9tas_hwbp_authoritative_fixed_delta_v1"
$remoteReport = "/data/local/tmp/a9tas_authoritative_fixed_delta_${gamePid}_$stamp.a9afd1"

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
        throw "Game process exited or changed PID during fixed-delta gate"
    }
    $tracerAfter = Get-TracerPidLine
    if ($tracerAfter -ne "TracerPid:`t0" -and $tracerAfter -ne "TracerPid: 0") {
        throw "Fixed-delta gate left a tracer attached: '$tracerAfter'"
    }
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Fixed-delta candidate push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Fixed-delta candidate chmod failed" }
if ((Get-RemoteSha256 $remoteBinary) -ne $pins[$binary]) {
    throw "Remote fixed-delta candidate hash differs after push"
}

Write-Host "AUTHORITATIVE_FIXED_DELTA_START pid=$gamePid frames=5 delta_writes=5"
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteReport $ack $PhysicsContextHex $MainObjectHex $FinalOwnerHex'" |
    Out-Host
$runCode = $LASTEXITCODE
Assert-StableGameAndDetach
if ($runCode -ne 0) {
    throw "Fixed-delta gate failed closed (exit=$runCode); game survived and TracerPid=0"
}
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Fixed-delta report chmod failed" }
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Fixed-delta report pull failed" }
python -B $validator $OutputPath --json-out $jsonOutput
if ($LASTEXITCODE -ne 0) { throw "A9AFD1 report validation failed" }
Assert-StableGameAndDetach
Write-Host "AUTHORITATIVE_FIXED_DELTA_LIVE_PASSED report=$OutputPath json=$jsonOutput TracerPid=0"
