# Guarded read-only resolver for the Android race lifecycle owner.
# Default mode is offline-only and never contacts ADB.

param(
    [ValidateSet("OfflineValidate", "ReadOnlyResolve")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputDirectory = "",
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeReadOnlyProcessScan,
    [switch]$ExecuteExactlyOneAttempt
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$buildScript = Join-Path $root "build-race-lifecycle-object-v1.ps1"
$candidate = Join-Path $root "build\race-lifecycle-object-v1\a9tas_race_lifecycle_object_check_v1_review_only"
$header = Join-Path $root "src\race_lifecycle_object_resolver_v1.h"
$source = Join-Path $root "src\race_lifecycle_object_check_v1.cpp"
$policy = Join-Path $root "tools\test_race_lifecycle_object_resolver_policy_v1.py"
$staticResolver = Join-Path $root "tools\resolve_race_lifecycle_static_v1.py"
$remoteCandidate = "/data/local/tmp/a9tas_race_lifecycle_object_check_v1"
$ack = "I_ACCEPT_RACE_LIFECYCLE_READ_ONLY_V1"

$pins = @{
    $candidate = "8dbb015f93ce4569d179c24983bce46cc03d9cf1c4c996fbf87e16a729302b59"
    $header = "a2d1c55d3cf24e4f3afea35d1aa5983e62e2ee29a08d199d7b293db4de0b472b"
    $source = "0cfc48afe402cc7c1cbd2cacdc712166998ae57e1027835fb91831d31146347f"
    $policy = "60706dfb3ea464ba68ca86c08ff45a98ce23e329ffc04116aac9b3d767beeb13"
    $buildScript = "4c79676755ad5dbc6ce8fa607d281c18116e99d4c16ca09205bd4f24644fccdf"
    $staticResolver = "550a40b4308186591e906efbaa4e92dd2464eaf260ded6d23b82ef0211ae71ed"
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments 2>&1) | Out-String).Trim()
}
function Invoke-AdbChecked([string[]]$Arguments, [string]$Label) {
    $result = & $AdbPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($result -join ' ')" }
    return $result
}
function Get-GamePid {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'pidof $package'")
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Get-StartTicks([int]$GamePid) {
    $stat = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed /proc/$GamePid/stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start-time field unavailable"
    }
    return [string]$fields[19]
}
function Get-LibraryBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $candidates = @(foreach ($line in $maps) {
        if ($line -match 'libAsphalt9\.so' -and
            $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) {
            $matches[1].ToLowerInvariant()
        }
    })
    $bases = @($candidates | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base" }
    return [string]$bases[0]
}
function Get-TracerPid([int]$GamePid) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($line -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid: $line" }
    return [int]$matches[1]
}
function Assert-RemoteHash([string]$Remote, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Remote'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Remote"
    }
}

foreach ($path in @($candidate, $header, $source, $policy, $buildScript,
                    $staticResolver)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing lifecycle resolver input: $path"
    }
    if ((Get-Sha $path) -ne $pins[$path]) {
        throw "Lifecycle resolver pin mismatch: $path"
    }
}

Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_race_lifecycle_static_v1
    $testCode = $LASTEXITCODE
    if ($testCode -eq 0) {
        python -B test_race_lifecycle_object_resolver_policy_v1.py $candidate
        $testCode = $LASTEXITCODE
    }
} finally { Pop-Location }
if ($testCode -ne 0) { throw "Lifecycle resolver offline validation failed" }

if ($Mode -eq "OfflineValidate") {
    Write-Output "RACE_LIFECYCLE_OBJECT_RUNNER_OFFLINE passed=1 controller_emitted=0 device_access=0"
    return
}

if (-not $AcknowledgeAncientRuinsZl1Countdown3Paused -or
    -not $AcknowledgeReadOnlyProcessScan -or
    -not $ExecuteExactlyOneAttempt) {
    throw "ReadOnlyResolve requires all three explicit acknowledgements"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found"
}
$devices = Invoke-AdbText @('devices')
if ($devices -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}
$gamePid = Get-GamePid
if ($gamePid -le 0) { throw "Game process unavailable" }
if ((Get-TracerPid $gamePid) -ne 0) { throw "Game already has a tracer" }
$startTicks = Get-StartTicks $gamePid
$base = Get-LibraryBaseHex $gamePid

Invoke-AdbChecked @('-s', $Device, 'push', $candidate, $remoteCandidate) `
    "push lifecycle resolver" | Out-Null
Invoke-AdbChecked @('-s', $Device, 'shell',
    "su -c 'chmod 700 $remoteCandidate'") "chmod lifecycle resolver" | Out-Null
Assert-RemoteHash $remoteCandidate $pins[$candidate]

$result = Invoke-AdbText @('-s', $Device, 'shell',
    "su -c '$remoteCandidate $gamePid $base $startTicks $ack'")
if ($LASTEXITCODE -ne 0 -or
    $result -notmatch 'RACE_LIFECYCLE_OBJECT_CHECK_V1 resolved=1' -or
    $result -notmatch 'countdown_candidates=1' -or
    $result -notmatch ' state=2 ' -or
    $result -notmatch 'gameplay_writes=0 ptrace_calls=0') {
    throw "Read-only lifecycle resolution failed: $result"
}
if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
    (Get-TracerPid $gamePid) -ne 0) {
    throw "Game identity/tracer changed during read-only lifecycle scan"
}
if ($OutputDirectory -eq "") { $OutputDirectory = Join-Path $root "evidence" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$report = Join-Path $OutputDirectory "race_lifecycle_object_${stamp}.txt"
$result | Set-Content -LiteralPath $report -Encoding ascii
$hash = Get-Sha $report
Write-Output $result
Write-Output "RACE_LIFECYCLE_OBJECT_READ_ONLY_PASS report=$report sha256=$hash"
