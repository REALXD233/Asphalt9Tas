param(
    [int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [switch]$Gate2Validated,
    [switch]$ReadOnlyRecordingValidated,
    [switch]$AcknowledgeOneSameBytesWrite
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $projectRoot `
    "build\staging\a9tas_hwbp_same_bytes_executor_v1"
$parser = Join-Path $projectRoot "tools\parse_same_bytes_audit_v1.py"
$expectedBinaryHash = `
    "16df9eacfb4b6c526b83f1249175340298f92232e8534860c83dbd5b6cbf7404"
$acknowledgement = "I_ACCEPT_ONE_SAME_BYTES_64_12_WRITE_V1"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_hwbp_same_bytes_executor_v1"

if (-not $Gate2Validated) {
    throw "Gate 2 live+Retry evidence must be acknowledged with -Gate2Validated."
}
if (-not $ReadOnlyRecordingValidated) {
    throw "The successful 300-frame A9NPS1 capture must be acknowledged with -ReadOnlyRecordingValidated."
}
if (-not $AcknowledgeOneSameBytesWrite) {
    throw "This performs one real 64+12 pwrite of identical bytes. Explicitly pass -AcknowledgeOneSameBytesWrite."
}
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 120000) {
    throw "TimeoutMs must be between 1000 and 120000"
}
if ($PhysicsContextHex -notmatch '^(0x)?[0-9a-fA-F]+$') {
    throw "Invalid PhysicsContextHex '$PhysicsContextHex'"
}
if (-not (Test-Path -LiteralPath $AdbPath)) { throw "ADB not found: $AdbPath" }
if (-not (Test-Path -LiteralPath $binary)) { throw "Executor not built: $binary" }
if (-not (Test-Path -LiteralPath $parser)) { throw "Parser not found: $parser" }
$actualHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLower()
if ($actualHash -ne $expectedBinaryHash) {
    throw "Executor hash mismatch: expected $expectedBinaryHash, got $actualHash"
}

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

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($OutputPath -eq "") {
    $OutputPath = Join-Path $projectRoot `
        "evidence\a9tas_same_bytes_audit_$stamp.a9sbt1"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) {
    throw "Local report path already exists: $OutputPath"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) `
    -Force | Out-Null
$remoteReport = "/data/local/tmp/a9tas_same_bytes_${gamePid}_$stamp.a9sbt1"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Executor push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Executor chmod failed" }

Write-Host "Arming one-shot same-bytes 64+12 transport..."
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteReport $acknowledgement $PhysicsContextHex'" `
    | Out-Host
$executionCode = $LASTEXITCODE
$tracerPid = (& $AdbPath -s $Device shell `
    "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") {
    throw "Executor did not detach cleanly: '$tracerPid'"
}
if ($executionCode -ne 0) {
    throw "Same-bytes executor failed closed (exit=$executionCode, TracerPid=0)"
}

& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Audit report pull failed" }
python -B $parser $OutputPath --require-supported
if ($LASTEXITCODE -ne 0) { throw "A9SBT1 report validation failed" }
$reportHash = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash.ToLower()
Write-Host "Audit report: $OutputPath"
Write-Host "SHA256 $reportHash"
