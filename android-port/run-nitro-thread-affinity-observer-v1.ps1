# GUARDED observation-only Nitro thread-affinity runner.
# This file is not live authorization and never sends the manual Space input.
param(
    [int]$DurationMs = 15000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ReportOutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeExactlyOneManualSpacePress,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeGuestMemoryReadOnly,
    [switch]$AcknowledgeDebugRegistersOnly,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\staging\a9tas_hwbp_nitro_thread_affinity_observer_v1"
$source = Join-Path $root "src\hwbp_nitro_thread_affinity_observer_v1.cpp"
$build = Join-Path $root "build-hwbp-nitro-thread-affinity-v1.ps1"
$parser = Join-Path $root "tools\parse_nitro_thread_affinity_report_v1.py"
$test = Join-Path $root "tools\test_nitro_thread_affinity_observer_v1.py"
$pins = @{
    $binary = "de931030942e29b002d176bc00b578f1a60d7c868fea0de682a053883c126797"
    $source = "a4573565373f2b906a48f99a96dc7f0a3ef6e27ba004e776a3a89f0a02133ce7"
    $build = "91deb19a1f904daa5acb39fb3ee8fa7c6021b777ba9acde7f60b2a6271fa6b68"
    $parser = "49fa399b3b716f46e34bc1e9c0517f857542a379961fe553d7ddbc16b49144e1"
    $test = "58c00aa146ad7c6c7aa7a189d1d6711a3eff5e0ce95fa8bc5336ba9db38ead1b"
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

Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_nitro_thread_affinity_observer_v1
    $testCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($testCode -ne 0) { throw "Nitro thread-affinity offline tests failed" }
if ($OfflineValidateOnly) {
    Write-Host "NITRO_THREAD_AFFINITY_OFFLINE_VALIDATION_OK"
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeExactlyOneManualSpacePress, "exactly one manual Space press during the observation window"),
    @($AcknowledgeNoPausedAttach, "that attach while paused is forbidden"),
    @($AcknowledgeGuestMemoryReadOnly, "that target memory is opened read-only"),
    @($AcknowledgeDebugRegistersOnly, "debug-register-only target writes"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if ($DurationMs -lt 5000 -or $DurationMs -gt 60000) {
    throw "DurationMs must be 5000..60000"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found"
}

$package = "com.aligames.kuang.kybc.aligames"
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($pidText -notmatch '^\d+$') { throw "Game PID unavailable" }
$gamePid = [int]$pidText
$tracer = (& $AdbPath -s $Device shell "grep '^TracerPid:' /proc/$gamePid/status").Trim()
if ($tracer -ne "TracerPid:`t0" -and $tracer -ne "TracerPid: 0") {
    throw "Game already traced: $tracer"
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
    $ReportOutputPath = Join-Path $root "evidence\a9tas_nitro_thread_affinity_$stamp.a9nta1"
}
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
if (Test-Path -LiteralPath $ReportOutputPath) { throw "Output already exists" }
$remoteBinary = "/data/local/tmp/a9tas_hwbp_nitro_thread_affinity_observer_v1"
$remoteReport = "/data/local/tmp/a9tas_nitro_thread_affinity_${gamePid}_$stamp.a9nta1"
& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }

Write-Host "NITRO_THREAD_AFFINITY_ARMED press Space exactly once manually"
& $AdbPath -s $Device shell `
    "su -c 'chmod 700 $remoteBinary; $remoteBinary $gamePid $baseHex $DurationMs $remoteReport'" |
    Out-Host
$runCode = $LASTEXITCODE
$afterPid = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
$tracerAfter = (& $AdbPath -s $Device shell "grep '^TracerPid:' /proc/$gamePid/status").Trim()
if ($afterPid -ne $pidText -or
    ($tracerAfter -ne "TracerPid:`t0" -and $tracerAfter -ne "TracerPid: 0")) {
    throw "Observer did not cleanly detach or the game process changed"
}
if ($runCode -ne 0) { throw "Observer failed closed (exit=$runCode)" }
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
& $AdbPath -s $Device pull $remoteReport $ReportOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9NTA1 report pull failed" }
python -B $parser $ReportOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9NTA1 verification failed" }
Write-Host "NITRO_THREAD_AFFINITY_PASSED report=$ReportOutputPath TracerPid=0"
