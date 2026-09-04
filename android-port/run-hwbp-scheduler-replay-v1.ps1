param(
    [Parameter(Mandatory = $true)]
    [string]$ReplayPath,
    [int]$TimeoutMs = 120000,
    [int]$FixedIntervalUs = 0,
    [string]$StateOutputPath = "",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [switch]$WaitForOwnerChange,
    [switch]$SafetyAck
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot "build\staging\a9tas_hwbp_scheduler_replay_v1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_scheduler_replay_v1"
$remoteReplay = "/data/local/tmp/a9tas_scheduler_replay_v1.a9spr1"
$remoteState = "/data/local/tmp/a9tas_scheduler_state_v1.bin"
$stateParser = Join-Path $projectRoot "tools\parse_vehicle_state_trace_v1.py"

if (-not $SafetyAck) {
    throw "SafetyAck is required because this test writes replay controls into the game"
}
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 600000) {
    throw "TimeoutMs must be between 1000 and 600000"
}
if ($FixedIntervalUs -ne 0 -and
    ($FixedIntervalUs -lt 1000 -or $FixedIntervalUs -gt 100000)) {
    throw "FixedIntervalUs must be 0 or between 1000 and 100000"
}
if ($WaitForOwnerChange -and $StateOutputPath -eq "") {
    throw "WaitForOwnerChange currently requires StateOutputPath"
}
$ReplayPath = [IO.Path]::GetFullPath($ReplayPath)
if (-not (Test-Path -LiteralPath $AdbPath)) { throw "ADB not found: $AdbPath" }
if (-not (Test-Path -LiteralPath $binary)) { throw "Replay controller not built: $binary" }
if (-not (Test-Path -LiteralPath $ReplayPath)) { throw "Replay not found: $ReplayPath" }

$deviceLine = & $AdbPath devices | Where-Object {
    $_ -match "^$([regex]::Escape($Device))\s+device$"
}
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }

$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') {
    throw "Game process is not running or pidof was ambiguous: '$pidText'"
}
$gamePid = [int]$pidText

$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) { throw "Failed to read game maps" }
$zeroOffsetBases = foreach ($line in $maps) {
    if ($line -notmatch 'libAsphalt9\.so') { continue }
    if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
        $mappingStart = [Convert]::ToUInt64($matches[1], 16)
        $fileOffset = [Convert]::ToUInt64($matches[2], 16)
        if ($fileOffset -eq 0) { $mappingStart }
    }
}
$zeroOffsetBases = @($zeroOffsetBases | Sort-Object -Unique)
if ($zeroOffsetBases.Count -ne 1) {
    throw "Expected exactly one zero-offset libAsphalt9 mapping, found $($zeroOffsetBases.Count)"
}
$baseHex = $zeroOffsetBases[0].ToString("x")

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Replay controller push failed" }
& $AdbPath -s $Device push $ReplayPath $remoteReplay | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Replay push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Replay controller chmod failed" }

if ($WaitForOwnerChange) {
    Write-Host "Arming Retry gate. Keep the race paused until ARMED, then press Retry once and do not pause during loading."
} else {
    Write-Host "Arming scheduler replay. Keep the race paused until ARMED appears."
}
if ($StateOutputPath -ne "") {
    $StateOutputPath = [IO.Path]::GetFullPath($StateOutputPath)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $StateOutputPath) | Out-Null
    if ($WaitForOwnerChange) {
        & $AdbPath -s $Device shell `
            "su -c '$remoteBinary $gamePid $baseHex $remoteReplay $TimeoutMs 0 0 $FixedIntervalUs $remoteState 1'" | Out-Host
    } else {
        & $AdbPath -s $Device shell `
            "su -c '$remoteBinary $gamePid $baseHex $remoteReplay $TimeoutMs 0 0 $FixedIntervalUs $remoteState'" | Out-Host
    }
} else {
    & $AdbPath -s $Device shell `
        "su -c '$remoteBinary $gamePid $baseHex $remoteReplay $TimeoutMs 0 0 $FixedIntervalUs'" | Out-Host
}
if ($LASTEXITCODE -ne 0) { throw "Scheduler replay failed" }
if ($StateOutputPath -ne "") {
    & $AdbPath -s $Device pull $remoteState $StateOutputPath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "State trace pull failed" }
    Write-Host "State trace: $StateOutputPath"
    python $stateParser $StateOutputPath
    if ($LASTEXITCODE -ne 0) { throw "State trace parser failed" }
}
