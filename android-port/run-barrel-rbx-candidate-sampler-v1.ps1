param(
    [int]$DurationMs = 10000,
    [int]$IntervalUs = 2000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [switch]$AutoEscapeResume
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_barrel_rbx_candidate_sampler_v1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_barrel_rbx_candidate_sampler_v1"

if ($DurationMs -lt 100 -or $DurationMs -gt 30000) {
    throw "DurationMs must be between 100 and 30000"
}
if ($IntervalUs -lt 250 -or $IntervalUs -gt 100000) {
    throw "IntervalUs must be between 250 and 100000"
}
if (-not (Test-Path -LiteralPath $AdbPath)) {
    throw "ADB not found: $AdbPath"
}
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Sampler not built: $binary"
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
if ($LASTEXITCODE -ne 0) { throw "Failed to read game maps" }
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
if ($LASTEXITCODE -ne 0) { throw "Sampler push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Sampler chmod failed" }

$samplerCommand =
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $IntervalUs'"
if ($AutoEscapeResume) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $stdoutPath = Join-Path $projectRoot `
        "build\barrel-rbx-auto-esc-$stamp.stdout.txt"
    $stderrPath = Join-Path $projectRoot `
        "build\barrel-rbx-auto-esc-$stamp.stderr.txt"
    $sampler = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', $samplerCommand) `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath
    $armed = $false
    $armDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $armDeadline -and -not $sampler.HasExited) {
        Start-Sleep -Milliseconds 50
        if ((Test-Path -LiteralPath $stdoutPath) -and
            (Get-Content -LiteralPath $stdoutPath -Raw) -match
                'BARREL_RBX_CANDIDATE_V1_ARMED') {
            $armed = $true
            break
        }
        $sampler.Refresh()
    }
    if (-not $armed) {
        if (-not $sampler.HasExited) { Stop-Process -Id $sampler.Id -Force }
        throw "Sampler did not publish ARMED before the resume deadline"
    }
    & $AdbPath -s $Device shell "input keyevent 111" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        if (-not $sampler.HasExited) { Stop-Process -Id $sampler.Id -Force }
        throw "Automatic ESC resume failed"
    }
    Write-Host "AUTO_ESCAPE_RESUME sent=1 after_armed=1"
    if (-not $sampler.WaitForExit($DurationMs + 20000)) {
        Stop-Process -Id $sampler.Id -Force
        throw "Sampler timed out after automatic ESC"
    }
    $sampler.Refresh()
    $result = @(Get-Content -LiteralPath $stdoutPath)
    $stderrText = if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath -Raw
    } else { "" }
    if ($sampler.ExitCode -ne 0) {
        throw "Sampler execution failed (exit=$($sampler.ExitCode)): $stderrText"
    }
} else {
    $result = & $AdbPath -s $Device shell $samplerCommand
    if ($LASTEXITCODE -ne 0) { throw "Sampler execution failed" }
}
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
