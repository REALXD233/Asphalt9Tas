param(
    [int]$DurationMs = 8000,
    [int]$WorkerTid = 0,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [ValidateSet("WorkerScope", "ExecutorAffinity")]
    [string]$Mode = "WorkerScope"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$isExecutorAffinity = $Mode -eq "ExecutorAffinity"
if ($isExecutorAffinity) {
    $binaryName = "a9tas_hwbp_executor_stack_affinity_v1"
    $parserName = "parse_hwbp_executor_stack_affinity_v1.py"
    $tracePrefix = "a9tas_hwbp_executor_stack_affinity_v1"
} else {
    $binaryName = "a9tas_hwbp_worker_stack_scope_observer_v1"
    $parserName = "parse_hwbp_worker_stack_scope_v1.py"
    $tracePrefix = "a9tas_hwbp_worker_stack_scope_v1"
}
$binary = Join-Path $projectRoot "build\staging\$binaryName"
$parser = Join-Path $projectRoot "tools\$parserName"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/$binaryName"

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
$pidText = (& $AdbPath -s $Device shell `
    "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') {
    throw "Game process is not running or pidof was ambiguous: '$pidText'"
}
$gamePid = [int]$pidText

if ($WorkerTid -eq 0) {
    $findWorker = "su -c 'grep -l `"^FrameThread 0$`" " +
        "/proc/$gamePid/task/*/comm 2>/dev/null'"
    $candidatePaths = & $AdbPath -s $Device shell $findWorker
    if ($LASTEXITCODE -ne 0 -and -not $candidatePaths) {
        throw "Failed to discover the physics worker task"
    }
    $candidates = @(
        $candidatePaths | ForEach-Object {
            if ($_ -match "/task/(\d+)/comm$") { [int]$matches[1] }
        } | Sort-Object -Unique
    )
    if ($candidates.Count -ne 1) {
        throw "Expected one 'FrameThread 0' task, found $($candidates.Count): $candidates"
    }
    $WorkerTid = $candidates[0]
}
$taskName = (& $AdbPath -s $Device shell `
    "su -c 'cat /proc/$gamePid/task/$WorkerTid/comm'").Trim()
if ($LASTEXITCODE -ne 0 -or $taskName -eq "") {
    throw "Worker task $WorkerTid is not alive in process $gamePid"
}
if ($taskName -ne "FrameThread 0") {
    throw "Task $WorkerTid has unexpected name '$taskName'"
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
$baseHex = $zeroOffsetBases[0].ToString("x")

if ($OutputPath -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $projectRoot `
        "evidence\$($tracePrefix)_$stamp.bin"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$mapsPath = "$OutputPath.maps.txt"
[IO.File]::WriteAllLines($mapsPath, [string[]]$maps)
$remoteTrace = "/data/local/tmp/$($tracePrefix)_$gamePid.bin"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Observer chmod failed" }

Write-Host "Validated physics worker $WorkerTid ($taskName)"
Write-Host "Arming host-only $Mode capture for $DurationMs ms..."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $remoteTrace $WorkerTid'" `
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
if ($isExecutorAffinity) {
    python $parser $OutputPath --minimum-hits 20 --require-supported
} else {
    python $parser $OutputPath --minimum-hits-per-field 20 --require-supported
}
if ($LASTEXITCODE -ne 0) {
    throw "Stack-scope semantic assessment failed (exit=$LASTEXITCODE)"
}
