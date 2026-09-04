param(
    [ValidateSet("OfflineValidate", "Observe")]
    [string]$Mode = "OfflineValidate",
    [int]$DurationMs = 2000,
    [int]$SampleMs = 10,
    [ValidateSet("inline-default", "car-physics")]
    [string]$ObserverProfile = "inline-default",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$PhysicsContextHex = "0",
    [string]$OutputPath = "",
    [switch]$AcknowledgeReadOnlyObservation
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root `
    "build\physics-interval-readonly-v1\a9tas_physics_interval_readonly_observer_v1"
$parser = Join-Path $root "tools\parse_physics_interval_readonly_v1.py"
$policy = Join-Path $root "tools\test_physics_interval_readonly_policy_v1.py"
$source = Join-Path $root "src\physics_interval_readonly_observer_v1.cpp"
$package = "com.aligames.kuang.kybc.aligames"
$remoteBinary = "/data/local/tmp/a9tas_physics_interval_readonly_observer_v1"

python -B $policy $source $PSCommandPath
if ($LASTEXITCODE -ne 0) { throw "Read-only observer policy failed" }
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Observer not built: $binary" }
if (-not (Test-Path -LiteralPath $parser -PathType Leaf)) { throw "Parser missing: $parser" }
if ($Mode -eq "OfflineValidate") {
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLower()
    Write-Output "PHYSICS_INTERVAL_READONLY_OFFLINE_VALID sha256=$hash device_access=0 ptrace=0 game_writes=0"
    exit 0
}
if (-not $AcknowledgeReadOnlyObservation) {
    throw "Observe requires explicit -AcknowledgeReadOnlyObservation"
}
if ($DurationMs -lt 100 -or $DurationMs -gt 30000) { throw "DurationMs out of range" }
if ($SampleMs -lt 1 -or $SampleMs -gt $DurationMs) { throw "SampleMs out of range" }
if ($PhysicsContextHex -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid PhysicsContextHex" }
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }

$deviceLine = & $AdbPath devices | Where-Object { $_ -match "^$([regex]::Escape($Device))\s+device$" }
if (-not $deviceLine) { throw "ADB device '$Device' is not connected" }
$pidText = (& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($LASTEXITCODE -ne 0 -or $pidText -notmatch '^\d+$') { throw "Game process unavailable or ambiguous: '$pidText'" }
$gamePid = [int]$pidText
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if ($LASTEXITCODE -ne 0) { throw "Failed to read game maps" }
$bases = foreach ($line in $maps) {
    if ($line -notmatch 'libAsphalt9\.so') { continue }
    if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
        if ([Convert]::ToUInt64($matches[2], 16) -eq 0) {
            [Convert]::ToUInt64($matches[1], 16)
        }
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "Expected one zero-offset libAsphalt9 mapping, found $($bases.Count)" }
$baseHex = $bases[0].ToString("x")
if ($OutputPath -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $OutputPath = Join-Path $root "evidence\a9tas_physics_interval_readonly_$stamp.a9pio1"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
$remoteTrace = "/data/local/tmp/a9tas_physics_interval_readonly_$gamePid.a9pio1"

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Observer push failed" }
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"
if ($LASTEXITCODE -ne 0) { throw "Observer chmod failed" }
& $AdbPath -s $Device shell `
    "su -c '$remoteBinary $gamePid $baseHex $DurationMs $SampleMs $remoteTrace $PhysicsContextHex'" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Read-only observation failed" }
& $AdbPath -s $Device pull $remoteTrace $OutputPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Receipt pull failed" }
$tracerPid = (& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim()
if ($tracerPid -ne "TracerPid:`t0" -and $tracerPid -ne "TracerPid: 0") { throw "Unexpected tracer: '$tracerPid'" }
python -B $parser $OutputPath --json --profile $ObserverProfile
if ($LASTEXITCODE -ne 0) { throw "Physics interval read-only semantic validation failed" }
$hash = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash.ToLower()
Write-Output "PHYSICS_INTERVAL_READONLY_LIVE_PASSED receipt=$OutputPath sha256=$hash TracerPid=0 ptrace=0 game_writes=0"
