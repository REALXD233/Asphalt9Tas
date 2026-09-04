param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_vehicle_state_resolver_check_v1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_vehicle_state_resolver_check_v1"

if (-not (Test-Path -LiteralPath $AdbPath)) {
    throw "ADB not found: $AdbPath"
}
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Resolver check not built: $binary"
}
$deviceLine = & $AdbPath devices | Where-Object {
    $_ -match "^$([regex]::Escape($Device))\s+device$"
}
if (-not $deviceLine) {
    throw "ADB device '$Device' is not connected"
}

$pidText = (& $AdbPath -s $Device shell `
    "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') {
    throw "Game process is not running or pidof was ambiguous: '$pidText'"
}
$gamePid = [int]$pidText
$maps = & $AdbPath -s $Device shell `
    "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to read game maps"
}
$zeroOffsetBases = foreach ($line in $maps) {
    if ($line -notmatch 'libAsphalt9\.so') { continue }
    if ($line -match `
        '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
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

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Resolver-check push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Resolver-check chmod failed" }

$result = & $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex'"
if ($LASTEXITCODE -ne 0) { throw "Resolver check failed" }
$result | Out-Host

if ($OutputPath -ne "") {
    $resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $resolvedOutput) `
        -Force | Out-Null
    [IO.File]::WriteAllLines($resolvedOutput, [string[]]$result)
    [IO.File]::WriteAllLines("$resolvedOutput.maps.txt", [string[]]$maps)
    Write-Host "Output: $resolvedOutput"
    Write-Host "Maps: $resolvedOutput.maps.txt"
}
