# Guarded observation-only direct_mode writer-affinity runner.
param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$DurationMs = 6000,
    [switch]$OfflineValidateOnly,
    [switch]$AcknowledgeNaturallyRunningRaceNoManualInput,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeGuestMemoryReadOnly,
    [switch]$AcknowledgeDebugRegistersOnly,
    [switch]$AcknowledgeNoRemoteCallsOrActivations,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$observer = Join-Path $root "build\staging\a9tas_hwbp_direct_mode_thread_affinity_v1"
$observerSource = Join-Path $root "src\hwbp_direct_mode_thread_affinity_v1.cpp"
$observerBuild = Join-Path $root "build-direct-mode-thread-affinity-v1.ps1"
$scanner = Join-Path $root "build\staging\a9tas_keyboard_bridge_scan_v7"
$pins = @{
    $observer = "38a16b8d070cdcb5056dc4fafe66e8d197ebd549fc69ff4479c3406a4f7d052d"
    $observerSource = "e220000127f40f38cda2d9941d4dbc6be834a3833fc88ddfff0d90871d5a37d0"
    $observerBuild = "327841d083a167eb13eb1b5069dff83211f4ae6a925fe0b44b14102dcc66425c"
    $scanner = "c8dbde4d967d255e75aefce2615147a1e684287f22de9750fea8722fac279974"
}

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return -join ($algorithm.ComputeHash($stream) | ForEach-Object {
                $_.ToString("x2")
            })
        } finally {
            $algorithm.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

foreach ($artifact in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing reviewed direct-mode affinity artifact: $artifact"
    }
    if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
        throw "Reviewed direct-mode affinity hash mismatch: $artifact"
    }
}
if ($DurationMs -lt 1000 -or $DurationMs -gt 30000) {
    throw "DurationMs must be 1000..30000"
}

$eventPattern = 'DIRECT_MODE_EVENT seq=(\d+) ns=(\d+) tid=(\d+) name=(.*?) value=(\d+) rip=0x([0-9a-fA-F]+) read_ok=1 regs_ok=1'
$donePattern = 'DIRECT_MODE_AFFINITY_V1_DONE events=(\d+) to_one=(\d+) to_zero=(\d+) initial_threads=(\d+) additions=(\d+) exited=(\d+) detached=(\d+) read_errors=0 ptrace_errors=0 unexpected_stops=0 final=(\d+) clean=1 activations=0'
if ($OfflineValidateOnly) {
    $sampleEvent = 'DIRECT_MODE_EVENT seq=0 ns=1234 tid=6119 name=FrameThread 0 value=1 rip=0x7ffff123 read_ok=1 regs_ok=1'
    $sampleDone = 'DIRECT_MODE_AFFINITY_V1_DONE events=720 to_one=360 to_zero=360 initial_threads=210 additions=0 exited=0 detached=210 read_errors=0 ptrace_errors=0 unexpected_stops=0 final=0 clean=1 activations=0'
    if ($sampleEvent -notmatch $eventPattern -or
        $matches[3] -ne '6119' -or $matches[4] -ne 'FrameThread 0' -or
        $sampleDone -notmatch $donePattern -or $matches[1] -ne '720') {
        throw "Direct-mode affinity parser selftest failed"
    }
    Write-Output "DIRECT_MODE_AFFINITY_V1_OFFLINE_VALIDATION_OK"
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRaceNoManualInput,
      "a naturally running race with no manual input"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeGuestMemoryReadOnly, "guest memory read-only scope"),
    @($AcknowledgeDebugRegistersOnly, "debug-register-only target writes"),
    @($AcknowledgeNoRemoteCallsOrActivations,
      "zero remote calls and zero activations"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

$gamePidText = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
if ($gamePidText -notmatch '^\d+$') { throw "Game PID unavailable" }
$gamePid = [int]$gamePidText
$tracerBefore = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($tracerBefore -ne "TracerPid:`t0" -and $tracerBefore -ne "TracerPid: 0") {
    throw "Game already traced: $tracerBefore"
}
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$bases = foreach ($line in $maps) {
    if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+.*libAsphalt9\.so' -and
        [Convert]::ToUInt64($matches[2], 16) -eq 0) {
        [Convert]::ToUInt64($matches[1], 16)
    }
}
$bases = @($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
$baseHex = $bases[0].ToString('x')

$remoteObserver = "/data/local/tmp/a9tas_hwbp_direct_mode_thread_affinity_v1"
$remoteScanner = "/data/local/tmp/a9tas_keyboard_bridge_scan_v7"
foreach ($pair in @(
    @($observer, $remoteObserver),
    @($scanner, $remoteScanner)
)) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair[1])" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteObserver $remoteScanner'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Device chmod failed" }

$scanLines = & $AdbPath -s $Device shell "su -c '$remoteScanner $gamePid $baseHex 0'" 2>&1
$scanCode = $LASTEXITCODE
$scanText = $scanLines -join "`n"
if ($scanCode -ne 0) { throw "Read-only command-owner scan failed" }
$ownerMatches = [regex]::Matches(
    $scanText,
    'ACTION07_COMMAND_PATH owner_adjustment=(-?\d+) owner=0x([0-9a-fA-F]+).*?direct_mode=(\d+)')
$owners = @($ownerMatches | ForEach-Object { $_.Groups[2].Value.ToLowerInvariant() } | Sort-Object -Unique)
if ($ownerMatches.Count -lt 1 -or $owners.Count -ne 1) {
    throw "Exactly one race command owner was not resolved"
}
foreach ($match in $ownerMatches) {
    if ($match.Groups[1].Value -ne '-6056' -or
        $match.Groups[3].Value -notin @('0', '1')) {
        throw "Command-owner adjustment/direct-mode value failed validation"
    }
}
$ownerHex = $owners[0]
Write-Output "DIRECT_MODE_AFFINITY_OWNER pid=$gamePid owner=0x$ownerHex base=0x$baseHex"

$observerLines = & $AdbPath -s $Device shell "su -c '$remoteObserver $gamePid $ownerHex $DurationMs'" 2>&1
$observerCode = $LASTEXITCODE
$observerText = $observerLines -join "`n"
$armedLine = $observerLines | Where-Object { $_ -match '^DIRECT_MODE_AFFINITY_V1_ARMED ' } | Select-Object -First 1
$doneLine = $observerLines | Where-Object { $_ -match '^DIRECT_MODE_AFFINITY_V1_DONE ' } | Select-Object -Last 1
if ($armedLine) { $armedLine | Out-Host }
$eventMatches = [regex]::Matches($observerText, $eventPattern)
$writerRows = foreach ($event in $eventMatches) {
    [pscustomobject]@{
        Tid = [int]$event.Groups[3].Value
        Name = $event.Groups[4].Value
        Value = [int]$event.Groups[5].Value
    }
}
foreach ($group in ($writerRows | Group-Object Tid,Name)) {
    $first = $group.Group[0]
    $ones = @($group.Group | Where-Object Value -eq 1).Count
    $zeros = @($group.Group | Where-Object Value -eq 0).Count
    Write-Output "DIRECT_MODE_WRITER tid=$($first.Tid) name=$($first.Name) events=$($group.Count) to_one=$ones to_zero=$zeros"
}
if ($doneLine) { $doneLine | Out-Host }
if ($observerCode -ne 0 -or $doneLine -notmatch $donePattern) {
    throw "Direct-mode affinity observer failed closed"
}
$events = [int64]$matches[1]
$toOne = [int64]$matches[2]
$toZero = [int64]$matches[3]
if ($events -le 0 -or $toOne -le 0 -or $eventMatches.Count -ne $events) {
    throw "Direct-mode writer evidence incomplete"
}

$finalPid = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
$tracerFinal = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($finalPid -ne $gamePidText -or
    ($tracerFinal -ne "TracerPid:`t0" -and $tracerFinal -ne "TracerPid: 0")) {
    throw "Direct-mode affinity cleanup verification failed"
}
Write-Output "DIRECT_MODE_AFFINITY_V1_PASSED pid=$gamePid owner=0x$ownerHex events=$events to_one=$toOne to_zero=$toZero activations=0 TracerPid=0"
