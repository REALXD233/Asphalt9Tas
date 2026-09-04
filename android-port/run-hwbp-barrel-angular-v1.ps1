param(
    [int]$DurationMs = 8000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [switch]$RequireCandidate,
    [switch]$AcknowledgeCaptureStall
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_barrel_angular_observer_v1"
$parser = Join-Path $projectRoot "tools\parse_hwbp_barrel_angular_v1.py"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_barrel_angular_observer_v1"

if (-not $AcknowledgeCaptureStall) {
    throw "This all-thread angular HWBP capture will visibly stall the game. Re-run only after explicit user permission with -AcknowledgeCaptureStall."
}
if ($DurationMs -lt 100 -or $DurationMs -gt 15000) {
    throw "DurationMs must be between 100 and 15000"
}
if (-not (Test-Path -LiteralPath $AdbPath)) { throw "ADB not found: $AdbPath" }
if (-not (Test-Path -LiteralPath $binary)) { throw "Observer not built: $binary" }
if (-not (Test-Path -LiteralPath $parser)) { throw "Parser not found: $parser" }

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
        $start = [Convert]::ToUInt64($matches[1], 16)
        $offset = [Convert]::ToUInt64($matches[2], 16)
        if ($offset -eq 0) { $start }
    }
}
$zeroOffsetBases = @($zeroOffsetBases | Sort-Object -Unique)
if ($zeroOffsetBases.Count -ne 1) {
    throw "Expected one zero-offset libAsphalt9 mapping, found $($zeroOffsetBases.Count)"
}
$baseHex = $zeroOffsetBases[0].ToString("x")

if ($OutputPath -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $projectRoot `
        "evidence\a9tas_hwbp_barrel_angular_v1_$stamp.bin"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$mapsPath = "$OutputPath.maps.txt"
[IO.File]::WriteAllLines($mapsPath, [string[]]$maps)
$remoteTrace = "/data/local/tmp/a9tas_hwbp_barrel_angular_v1_$gamePid.bin"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Observer chmod failed" }

Write-Host "Arming host-only barrel/angular capture for $DurationMs ms..."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $remoteTrace'" `
    | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer execution failed" }

& $AdbPath -s $Device pull $remoteTrace $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Trace pull failed" }
$tracerPid = (& $AdbPath -s $Device shell `
    "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
    throw "Observer did not detach cleanly: '$tracerPid'"
}

Write-Host "Trace: $OutputPath"
Write-Host "Maps: $mapsPath"
$parserArguments = @($parser, $OutputPath)
if ($RequireCandidate) { $parserArguments += "--require-candidate" }
python @parserArguments
if ($LASTEXITCODE -ne 0) {
    throw "Barrel/angular semantic assessment failed (exit=$LASTEXITCODE)"
}
