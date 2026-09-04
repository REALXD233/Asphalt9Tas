# GUARDED observation-only runner for the game-owned action submission route.
# It never sends the manual Space input and never calls a game function.
param(
    [ValidateSet("OfflineValidateOnly", "Capture")]
    [string]$Mode = "OfflineValidateOnly",
    [int]$DurationMs = 15000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ReportOutputPath = "",
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeExactlyOneManualSpacePress,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeGuestMemoryReadOnly,
    [switch]$AcknowledgeDebugRegistersOnly,
    [switch]$AcknowledgeNoAutomatedInput,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root `
    "build\game-action-submission-affinity-v1\a9tas_hwbp_game_action_submission_affinity_v1"
$source = Join-Path $root "src\hwbp_game_action_submission_affinity_v1.cpp"
$build = Join-Path $root "build-hwbp-game-action-submission-affinity-v1.ps1"
$parser = Join-Path $root "tools\parse_game_action_submission_affinity_v1.py"
$parserTest = Join-Path $root "tools\test_parse_game_action_submission_affinity_v1.py"
$policyTest = Join-Path $root "tools\test_game_action_submission_affinity_build_policy_v1.py"
$pins = @{
    $binary = "997183c50d6641004f19475e7253e9fcb455489c76f2033adabdfa2034b0f9e2"
    $source = "5ee43fea0c386e7e52181f9ca23deedaf9ed7ed417069243e0a64b0d7e53ddfd"
    $build = "5a0a2f956d6c14744261c87a061e0b36694d6582abf7e19f36bc0453f993f6c5"
    $parser = "bf1ed625464c60b7cd869eac8304be46aa33f82ed3aa9c531e3f6b3ccffa91e1"
    $parserTest = "1d2169328957504bf725947ae9e152dd7c6729d02cfbeb26d217185d4f3d1531"
    $policyTest = "b0de7ea91f41a48be0ff51cc334eabd113a5ea26f5aeab180343dc648b348ed1"
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
    python -B -m unittest `
        test_parse_game_action_submission_affinity_v1 `
        test_game_action_submission_affinity_build_policy_v1
    $testCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($testCode -ne 0) {
    throw "game action submission-affinity offline tests failed"
}
if ($Mode -eq "OfflineValidateOnly") {
    Write-Host `
        "GAME_ACTION_SUBMISSION_AFFINITY_OFFLINE_VALIDATION_OK device_access=0 attached=0 input_sent=0"
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeExactlyOneManualSpacePress, "exactly one manual Space press during the capture"),
    @($AcknowledgeNoPausedAttach, "that attach while paused is forbidden"),
    @($AcknowledgeGuestMemoryReadOnly, "that game memory is opened read-only"),
    @($AcknowledgeDebugRegistersOnly, "debug-register-only target writes"),
    @($AcknowledgeNoAutomatedInput, "that the runner sends no automated input"),
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
$tracer = (& $AdbPath -s $Device shell `
    "grep '^TracerPid:' /proc/$gamePid/status").Trim()
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
    $ReportOutputPath = Join-Path $root `
        "evidence\a9tas_game_action_submission_affinity_$stamp.a9asa1.txt"
}
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
if (Test-Path -LiteralPath $ReportOutputPath) {
    throw "Output already exists"
}
$remoteBinary = "/data/local/tmp/a9tas_hwbp_game_action_submission_affinity_v1"
$remoteReport = "/data/local/tmp/a9tas_game_action_submission_${gamePid}_$stamp.a9asa1.txt"
& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }

Write-Host `
    "GAME_ACTION_SUBMISSION_AFFINITY_ARMED press Space exactly once manually; runner sends no input"
& $AdbPath -s $Device shell `
    "su -c 'chmod 700 $remoteBinary; $remoteBinary $gamePid $baseHex $DurationMs $remoteReport'" |
    Out-Host
$runCode = $LASTEXITCODE
$afterPid = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
$tracerAfter = (& $AdbPath -s $Device shell `
    "grep '^TracerPid:' /proc/$gamePid/status").Trim()
if ($afterPid -ne $pidText -or
    ($tracerAfter -ne "TracerPid:`t0" -and $tracerAfter -ne "TracerPid: 0")) {
    throw "Observer did not cleanly detach or the game process changed"
}
if ($runCode -ne 0) { throw "Observer failed closed (exit=$runCode)" }
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
& $AdbPath -s $Device pull $remoteReport $ReportOutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "A9ASA1 transcript pull failed" }
python -B $parser $ReportOutputPath
if ($LASTEXITCODE -ne 0) { throw "A9ASA1 strict verification failed" }
Write-Host `
    "GAME_ACTION_SUBMISSION_AFFINITY_PASSED report=$ReportOutputPath TracerPid=0 input_sent=0"
