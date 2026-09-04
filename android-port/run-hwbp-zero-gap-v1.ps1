param(
    [int]$DurationMs = 30000,
    [int]$MainTid = 0,
    [int]$WorkerTid = 0,
    [ValidateSet("position", "rotation")]
    [string]$PoseKind = "position",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot "build\staging\a9tas_hwbp_zero_gap_observer_v1"
$parser = Join-Path $projectRoot "tools\parse_hwbp_zero_gap_v1.py"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_zero_gap_observer_v1"

if ($DurationMs -lt 100 -or $DurationMs -gt 300000) {
    throw "DurationMs must be between 100 and 300000"
}
if ($MainTid -le 0 -or $WorkerTid -le 0) {
    throw "MainTid and WorkerTid must be explicit positive task IDs"
}
if (-not (Test-Path -LiteralPath $AdbPath)) {
    throw "ADB not found: $AdbPath"
}
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Observer not built: $binary"
}
if (-not (Test-Path -LiteralPath $parser)) {
    throw "Parser not found: $parser"
}

$deviceLine = & $AdbPath devices | Where-Object {
    $_ -match "^$([regex]::Escape($Device))\s+device$"
}
if (-not $deviceLine) {
    throw "ADB device '$Device' is not connected"
}

$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') {
    throw "Game process is not running or pidof was ambiguous: '$pidText'"
}
$gamePid = [int]$pidText
foreach ($tid in @($MainTid, $WorkerTid)) {
    $taskName = (& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/task/$tid/comm'").Trim()
    if ($LASTEXITCODE -ne 0 -or $taskName -eq "") {
        throw "Task $tid is not alive in game process $gamePid"
    }
    Write-Host "Validated task $tid ($taskName)"
}

$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to read game maps"
}
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

if ($OutputPath -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $projectRoot "evidence\a9tas_hwbp_zero_gap_v1_$stamp.bin"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$mapsOutputPath = "$OutputPath.maps.txt"
[IO.File]::WriteAllLines($mapsOutputPath, [string[]]$maps)
$remoteTrace = "/data/local/tmp/a9tas_hwbp_zero_gap_v1_$gamePid.bin"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Observer chmod failed" }

Write-Host "Arming read-only zero-gap capture for $DurationMs ms..."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $remoteTrace $MainTid $WorkerTid $PoseKind'" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer execution failed" }

& $AdbPath -s $Device pull $remoteTrace $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Trace pull failed" }

Write-Host "Trace: $OutputPath"
Write-Host "Maps: $mapsOutputPath"
python $parser $OutputPath --minimum-cycles 20
if ($LASTEXITCODE -ne 0) { throw "Trace parser failed" }
