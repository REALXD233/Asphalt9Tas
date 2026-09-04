param(
    [int]$DurationMs = 10000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [int]$TargetFrames = 300,
    [switch]$AcknowledgeCaptureStall,
    [switch]$Gate2Validated
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_native_physics_recorder_v1"
$parser = Join-Path $projectRoot "tools\native_physics_recording_v1.py"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_native_physics_recorder_v1"

if (-not $Gate2Validated) {
    throw "A9NPS1 capture is locked until the accepted Gate 2 live+Retry evidence is acknowledged with -Gate2Validated."
}
if (-not $AcknowledgeCaptureStall) {
    throw "This four-address capture will visibly stall the game. Re-run only with explicit user permission and -AcknowledgeCaptureStall."
}
if ($DurationMs -lt 100 -or $DurationMs -gt 120000) {
    throw "DurationMs must be between 100 and 120000"
}
if ($TargetFrames -lt 1 -or $TargetFrames -gt 36000) {
    throw "TargetFrames must be between 1 and 36000"
}
if ($PhysicsContextHex -notmatch '^(0x)?[0-9a-fA-F]+$') {
    throw "Invalid PhysicsContextHex '$PhysicsContextHex'"
}
if (-not (Test-Path -LiteralPath $AdbPath)) { throw "ADB not found: $AdbPath" }
if (-not (Test-Path -LiteralPath $binary)) { throw "Recorder not built: $binary" }
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
$bases = foreach ($line in $maps) {
    if ($line -notmatch 'libAsphalt9\.so') { continue }
    if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
        $start = [Convert]::ToUInt64($matches[1], 16)
        $offset = [Convert]::ToUInt64($matches[2], 16)
        if ($offset -eq 0) { $start }
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "Expected one zero-offset libAsphalt9 mapping" }
$baseHex = $bases[0].ToString("x")

if ($OutputPath -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $projectRoot "evidence\a9tas_native_physics_$stamp.a9nps1"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
$remoteTrace = "/data/local/tmp/a9tas_native_physics_$gamePid.a9nps1"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recorder push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Recorder chmod failed" }
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $remoteTrace $PhysicsContextHex $TargetFrames'" `
    | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recorder execution failed" }
& $AdbPath -s $Device pull $remoteTrace $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recording pull failed" }
$tracerPid = (& $AdbPath -s $Device shell `
    "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
    throw "Recorder did not detach cleanly: '$tracerPid'"
}
python -B $parser $OutputPath --require-runtime-safe
if ($LASTEXITCODE -ne 0) { throw "A9NPS1 validation failed" }
Write-Host "Recording: $OutputPath"
