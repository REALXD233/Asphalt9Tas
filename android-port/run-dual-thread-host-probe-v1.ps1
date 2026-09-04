# Guarded DTA-0 runner. It uses the process-bound writer identity from the
# completed live affinity gate, holds it at direct_mode=1 by read-only polling
# plus PTRACE_INTERRUPT, then performs one host gettid on FrameThread 0.
param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$HoldTimeoutMs = 8000,
    [switch]$OfflineValidateOnly,
    [switch]$AcknowledgeNaturallyRunningRaceNoManualInput,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeWriterHoldRisk,
    [switch]$AcknowledgeExactlyOneHostGettid,
    [switch]$AcknowledgeNoGuestCallsActionsActivationsOrPhysicsWrites,
    [switch]$AcknowledgeDualThreadPtraceStallRisk,
    [switch]$AcknowledgeProcessBoundWriterEvidence
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$controller = Join-Path $root "build\same-thread-probe-v1\a9tas_dual_thread_host_probe_controller_v1"
$controllerSource = Join-Path $root "src\dual_thread_host_probe_controller_v1.cpp"
$frameSource = Join-Path $root "src\same_thread_frame_probe_controller_v1.cpp"
$commonSource = Join-Path $root "src\same_thread_probe_controller_v1.cpp"
$injectorSource = Join-Path $root "src\injector.cpp"
$controllerBuild = Join-Path $root "build-same-thread-probe-v1.ps1"
$scanner = Join-Path $root "build\staging\a9tas_keyboard_bridge_scan_v7"
$pins = @{
    $controller = "8682bb3803874420689b78ab12b2459021dfe9f06740f9945444c9f9cf15b2b8"
    $controllerSource = "9b7d838ebcf3e5d36da9e4161d6047dc25d744e671378684c805b5fe34a31199"
    $frameSource = "a21623b828c087f71fbd880e9f92cee42e80be7b7104467fb91933911f0c5b75"
    $commonSource = "14ab2e90b905bc2cd5f3dfff8e054d7fbdc2762a970963c32fcf9bd5758f1cc7"
    $injectorSource = "bcc29f5f9470332abfcc87b1c8ae715a12633f79de93667c453c1097856c74b2"
    $controllerBuild = "5a4d4db64c3da94378e80936635ccbd02d5676dd51807be79dbbd5d2c4a1325f"
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
        } finally { $algorithm.Dispose() }
    } finally { $stream.Dispose() }
}

foreach ($artifact in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing reviewed DTA-0 artifact: $artifact"
    }
    if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
        throw "Reviewed DTA-0 hash mismatch: $artifact"
    }
}
if ($HoldTimeoutMs -lt 1000 -or $HoldTimeoutMs -gt 15000) {
    throw "HoldTimeoutMs must be 1000..15000"
}

$dualResultPattern = 'DUAL_THREAD_HOST_PROBE_V1_RESULT passed=(\d+) pid=(\d+) writer_tid=(\d+) writer_name=(.*?) frame_tid=(\d+) hits=(\d+) writer_hit=(\d+) host_tid=(\d+) host_ok=(\d+) hold_ns=(\d+) unexpected_stops=(\d+) alive=(\d+) tracer_pid=(\d+) guest_calls=(\d+) activations=(\d+)'
if ($OfflineValidateOnly) {
    $sampleDual = 'DUAL_THREAD_HOST_PROBE_V1_RESULT passed=1 pid=4607 writer_tid=4701 writer_name=Thread-718 frame_tid=6119 hits=1 writer_hit=1 host_tid=6119 host_ok=1 hold_ns=2000000 unexpected_stops=0 alive=1 tracer_pid=0 guest_calls=0 activations=0'
    $dual = [regex]::Match($sampleDual, $dualResultPattern)
    if (-not $dual.Success -or $dual.Groups[3].Value -ne '4701' -or
        $dual.Groups[5].Value -ne $dual.Groups[8].Value -or
        $dual.Groups[14].Value -ne '0' -or $dual.Groups[15].Value -ne '0') {
        throw "DTA-0 parser selftest failed"
    }
    Write-Output "DUAL_THREAD_HOST_PROBE_V1_OFFLINE_VALIDATION_OK"
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRaceNoManualInput,
      "a naturally running race with no manual input"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeWriterHoldRisk,
      "the risk of briefly holding the direct-mode writer"),
    @($AcknowledgeExactlyOneHostGettid,
      "exactly one host gettid on FrameThread 0"),
    @($AcknowledgeNoGuestCallsActionsActivationsOrPhysicsWrites,
      "zero guest calls, actions, activations, and physics writes"),
    @($AcknowledgeDualThreadPtraceStallRisk,
      "the dual-thread ptrace stall risk"),
    @($AcknowledgeProcessBoundWriterEvidence,
      "the PID-4607/TID-4701 process-bound writer evidence")
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

$remoteController = "/data/local/tmp/a9tas_dual_thread_host_probe_controller_v1"
$remoteScanner = "/data/local/tmp/a9tas_keyboard_bridge_scan_v7"
foreach ($pair in @(
    @($controller, $remoteController),
    @($scanner, $remoteScanner)
)) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair[1])" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteController $remoteScanner'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Device chmod failed" }

$selftestLines = & $AdbPath -s $Device shell "su -c '$remoteController --selftest'" 2>&1
$selftestCode = $LASTEXITCODE
$selftestLines | Out-Host
if ($selftestCode -ne 0 -or
    ($selftestLines -join "`n") -notmatch 'DUAL_THREAD_HOST_PROBE_V1_SELFTEST passed=1 stage_timeout_ms=250') {
    throw "DTA-0 device selftest failed"
}

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
        throw "Command-owner adjustment/direct-mode precondition failed"
    }
}
$ownerHex = $owners[0]

$reviewedWriterPid = 4607
$writerTid = '4701'
if ($gamePid -ne $reviewedWriterPid) {
    throw "Current PID is outside the reviewed writer-affinity evidence"
}
$writerName = ((& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/task/$writerTid/comm'" 2>$null) -join '').Trim()
$writerStatus = (& $AdbPath -s $Device shell "su -c 'grep -E `"^(Tgid|TracerPid):`" /proc/$gamePid/task/$writerTid/status'" 2>$null) -join "`n"
if ([string]::IsNullOrWhiteSpace($writerName) -or
    $writerName -eq 'FrameThread 0' -or
    $writerStatus -notmatch "Tgid:\s+$gamePid" -or
    $writerStatus -notmatch 'TracerPid:\s+0') {
    throw "Process-bound writer identity is no longer valid"
}
Write-Output "DTA0_WRITER_REVALIDATED pid=$gamePid owner=0x$ownerHex writer_tid=$writerTid writer_name=$writerName source=process_bound_live_evidence"

$afterAffinityPid = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
$tracerAfterAffinity = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($afterAffinityPid -ne $gamePidText -or
    ($tracerAfterAffinity -ne "TracerPid:`t0" -and $tracerAfterAffinity -ne "TracerPid: 0")) {
    throw "Process changed or tracer leaked after writer revalidation"
}

$ack = "I_ACCEPT_DUAL_THREAD_HOST_GETTID_PROBE_V1"
$dualLines = & $AdbPath -s $Device shell "su -c '$remoteController $gamePid $ownerHex $writerTid $HoldTimeoutMs 0 $ack'" 2>&1
$dualCode = $LASTEXITCODE
$dualLines | Out-Host
$dual = [regex]::Match(($dualLines -join "`n"), $dualResultPattern)
if ($dualCode -ne 0 -or -not $dual.Success -or
    $dual.Groups[1].Value -ne '1' -or
    $dual.Groups[2].Value -ne $gamePidText -or
    $dual.Groups[3].Value -ne $writerTid -or
    $dual.Groups[4].Value -ne $writerName -or
    $dual.Groups[5].Value -ne $dual.Groups[8].Value -or
    $dual.Groups[7].Value -ne '1' -or $dual.Groups[9].Value -ne '1' -or
    $dual.Groups[11].Value -ne '0' -or $dual.Groups[12].Value -ne '1' -or
    $dual.Groups[13].Value -ne '0' -or $dual.Groups[14].Value -ne '0' -or
    $dual.Groups[15].Value -ne '0') {
    throw "DTA-0 dual-thread host probe failed closed"
}

$finalPid = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
$tracerFinal = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($finalPid -ne $gamePidText -or
    ($tracerFinal -ne "TracerPid:`t0" -and $tracerFinal -ne "TracerPid: 0")) {
    throw "DTA-0 cleanup verification failed"
}
Write-Output "DUAL_THREAD_HOST_PROBE_V1_PASSED pid=$gamePid writer_tid=$writerTid writer_name=$writerName frame_tid=$($dual.Groups[5].Value) hold_ns=$($dual.Groups[10].Value) guest_calls=0 activations=0 TracerPid=0"
