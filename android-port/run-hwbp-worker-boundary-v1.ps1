param(
    [int]$DurationMs = 30000,
    [int]$WorkerTid = 0,
    [string]$ActiveAddressHex = "",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
throw "QUARANTINED: this runner depends on a forbidden guest ARM64 inline hook. Use run-hwbp-worker-stack-scope-v1.ps1 instead."
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_worker_boundary_observer_v1"
$parser = Join-Path $projectRoot "tools\parse_hwbp_worker_boundary_v1.py"
$package = "com.aligames.kuang.kybc.aligames"
$statusPath = "/data/user/0/$package/files/a9tas-worker-boundary-v1.status"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_worker_boundary_observer_v1"

if ($DurationMs -lt 100 -or $DurationMs -gt 300000) {
    throw "DurationMs must be between 100 and 300000"
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

$statusLines = & $AdbPath -s $Device shell "su -c 'cat $statusPath'"
if ($LASTEXITCODE -ne 0) { throw "Worker-boundary payload status is unavailable" }
$status = @{}
foreach ($line in $statusLines) {
    if ($line -match '^([^=]+)=(.*)$') { $status[$matches[1]] = $matches[2].Trim() }
}
if ($status['protocol'] -ne 'worker-boundary-v1' -or
    $status['state'] -ne 'observing' -or $status['installed'] -ne '1' -or
    $status['observation_only'] -ne '1' -or $status['vehicle_writes'] -ne '0') {
    throw "Worker-boundary payload status failed closed: $($statusLines -join '; ')"
}
if ($ActiveAddressHex -eq "") { $ActiveAddressHex = $status['active_address'] }
if ($WorkerTid -eq 0 -and $status['first_tid'] -match '^\d+$') {
    $WorkerTid = [int]$status['first_tid']
}
if ($ActiveAddressHex -notmatch '^0x[0-9a-fA-F]+$' -or $WorkerTid -le 0) {
    throw "Active address or worker TID is not ready; enter a race first"
}
$activeArgument = $ActiveAddressHex.Substring(2)
$taskName = (& $AdbPath -s $Device shell `
    "su -c 'cat /proc/$gamePid/task/$WorkerTid/comm'").Trim()
if ($LASTEXITCODE -ne 0 -or $taskName -eq "") {
    throw "Worker task $WorkerTid is not alive in process $gamePid"
}

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
$baseHex = $zeroOffsetBases[0].ToString('x')

if ($OutputPath -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $projectRoot `
        "evidence\a9tas_hwbp_worker_boundary_v1_$stamp.bin"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$mapsPath = "$OutputPath.maps.txt"
[IO.File]::WriteAllLines($mapsPath, [string[]]$maps)
$remoteTrace = "/data/local/tmp/a9tas_hwbp_worker_boundary_v1_$gamePid.bin"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Observer chmod failed" }

Write-Host "Validated worker $WorkerTid ($taskName), active=$ActiveAddressHex"
Write-Host "Arming read-only worker-boundary capture for $DurationMs ms..."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $remoteTrace $WorkerTid $activeArgument'" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer execution failed" }
& $AdbPath -s $Device pull $remoteTrace $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Trace pull failed" }
Write-Host "Trace: $OutputPath"
Write-Host "Maps: $mapsPath"
python $parser $OutputPath --minimum-pairs 20
if ($LASTEXITCODE -ne 0) { throw "Trace parser failed" }
