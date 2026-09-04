# Guarded ST-3 runner. It performs only host/guest gettid probes on
# FrameThread 0. It contains no Nitro resolver or activation call.
param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 8000,
    [switch]$OfflineValidateOnly,
    [switch]$AcknowledgeSt1St2SameProcessRevalidation,
    [switch]$AcknowledgeNaturallyRunningRaceNoManualInput,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeExactlyOneHostAndOneGuestGettid,
    [switch]$AcknowledgeZeroActionsActivationsAndPhysicsWrites,
    [switch]$AcknowledgeFrameThreadShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$payload = Join-Path $root "build\same-thread-probe-v1\liba9tas_same_thread_probe_v1.so"
$bootstrap = Join-Path $root "build\same-thread-probe-v1\liba9tas_bootstrap_same_thread_probe_v1.so"
$st2Controller = Join-Path $root "build\same-thread-probe-v1\a9tas_same_thread_probe_controller_v1"
$frameController = Join-Path $root "build\same-thread-probe-v1\a9tas_same_thread_frame_probe_controller_v1"
$scanner = Join-Path $root "build\staging\a9tas_keyboard_bridge_scan_v7"
$frameSource = Join-Path $root "src\same_thread_frame_probe_controller_v1.cpp"
$st2Source = Join-Path $root "src\same_thread_probe_controller_v1.cpp"
$payloadSource = Join-Path $root "src\payload_same_thread_probe_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap.cpp"
$buildScript = Join-Path $root "build-same-thread-probe-v1.ps1"

$pins = @{
    $payload = "f68c3396695ec715428a2cb4508b2943f1975be745b5e9d67e78e34ef3d00082"
    $bootstrap = "af5f4d4e8767af6f0e60d21a46e1d6996b83c8628e66593438ddea3032e4aa88"
    $st2Controller = "a689b498bb369b628dc379927e20cee1571fab6a01d4ef419d2cdf34f5f9382e"
    $frameController = "37e3864c50be96dd1be8acf7c7acbca43b4a701641e90f5bf7b9fbb58ba4b65b"
    $scanner = "c8dbde4d967d255e75aefce2615147a1e684287f22de9750fea8722fac279974"
    $frameSource = "a21623b828c087f71fbd880e9f92cee42e80be7b7104467fb91933911f0c5b75"
    $st2Source = "14ab2e90b905bc2cd5f3dfff8e054d7fbdc2762a970963c32fcf9bd5758f1cc7"
    $payloadSource = "a63c565041fe7cfe5b6c1bc00978ea6b93807070025f1c1dfc0fc4f5084e4c02"
    $bootstrapSource = "f2ca03a0ffbf15691f55d9e6e2a1c74c6ade01714d84d56e844c2388ee1f5ecd"
    $buildScript = "5a4d4db64c3da94378e80936635ccbd02d5676dd51807be79dbbd5d2c4a1325f"
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
        throw "Missing reviewed ST-3 artifact: $artifact"
    }
    if ((Get-Sha256 $artifact) -ne $pins[$artifact]) {
        throw "Reviewed ST-3 artifact hash mismatch: $artifact"
    }
}
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 15000) {
    throw "TimeoutMs must be 1000..15000"
}
if ($OfflineValidateOnly) {
    $sampleSt2 = "SAME_THREAD_PROBE_V1_RESULT passed=1 pid=4607 tid=4613 status=1 trampoline=0x7ffff4533560 returned_tid=4613 alive=1 tracer_pid=0"
    if ($sampleSt2 -notmatch 'SAME_THREAD_PROBE_V1_RESULT passed=1 pid=4607 tid=(\d+) status=1 trampoline=0x([0-9a-fA-F]+) returned_tid=\1 alive=1 tracer_pid=0' -or
        $matches[2] -ne '7ffff4533560') {
        throw "Offline ST-2 parser selftest failed"
    }
    $sampleOwner = "ACTION07_COMMAND_PATH owner_adjustment=-6056 owner=0x7ffefa95e800 interface_vtable=0x7ffefad39f40 queue_count=0 direct_mode=0"
    $sampleOwnerMatches = [regex]::Matches(
        $sampleOwner,
        'ACTION07_COMMAND_PATH owner_adjustment=(-?\d+) owner=0x([0-9a-fA-F]+).*?direct_mode=(\d+)')
    if ($sampleOwnerMatches.Count -ne 1 -or
        $sampleOwnerMatches[0].Groups[1].Value -ne '-6056' -or
        $sampleOwnerMatches[0].Groups[2].Value -ne '7ffefa95e800' -or
        $sampleOwnerMatches[0].Groups[3].Value -ne '0') {
        throw "Offline command-owner parser selftest failed"
    }
    $sampleFrame = "FRAME_THREAD_PROBE_V1_RESULT passed=1 pid=4607 tid=4711 hits=1 accepted=1 host_tid=4711 host_ok=1 guest_tid=4711 guest_ok=1 unexpected_stops=0 alive=1 tracer_pid=0 activations=0"
    if ($sampleFrame -notmatch 'FRAME_THREAD_PROBE_V1_RESULT passed=1 pid=4607 tid=(\d+) hits=(\d+) accepted=1 host_tid=\1 host_ok=1 guest_tid=\1 guest_ok=1 unexpected_stops=0 alive=1 tracer_pid=0 activations=0') {
        throw "Offline ST-3 parser selftest failed"
    }
    Write-Output "ST3_FRAME_THREAD_PROBE_V1_OFFLINE_VALIDATION_OK"
    return
}

foreach ($gate in @(
    @($AcknowledgeSt1St2SameProcessRevalidation,
      "ST-1/ST-2 same-process revalidation"),
    @($AcknowledgeNaturallyRunningRaceNoManualInput,
      "a naturally running race with no manual input"),
    @($AcknowledgeNoPausedAttach, "that paused attach is forbidden"),
    @($AcknowledgeExactlyOneHostAndOneGuestGettid,
      "exactly one host and one guest gettid on FrameThread 0"),
    @($AcknowledgeZeroActionsActivationsAndPhysicsWrites,
      "zero actions, activations, and physics writes"),
    @($AcknowledgeFrameThreadShortPtraceStallRisk,
      "the short FrameThread ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}

$remotePayload = "/data/local/tmp/liba9tas_same_thread_probe_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_same_thread_probe_v1.so"
$remoteSt2 = "/data/local/tmp/a9tas_same_thread_probe_controller_v1"
$remoteFrame = "/data/local/tmp/a9tas_same_thread_frame_probe_controller_v1"
$remoteScanner = "/data/local/tmp/a9tas_keyboard_bridge_scan_v7"
$gamePidText = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
if ($gamePidText -notmatch '^\d+$') { throw "Game PID unavailable" }
$gamePid = [int]$gamePidText
$tracerBefore = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($tracerBefore -ne "TracerPid:`t0" -and $tracerBefore -ne "TracerPid: 0") {
    throw "Game already traced: $tracerBefore"
}

foreach ($remotePin in @(
    @($remotePayload, $pins[$payload]),
    @($remoteBootstrap, $pins[$bootstrap])
)) {
    $hashLine = ((& $AdbPath -s $Device shell "su -c 'sha256sum $($remotePin[0])'" 2>$null) -join '').Trim()
    if ($hashLine -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $remotePin[1]) {
        throw "Device artifact hash mismatch: $($remotePin[0])"
    }
}
$maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
if (-not (($maps -join "`n").Contains($remotePayload)) -or
    -not (($maps -join "`n").Contains($remoteBootstrap))) {
    throw "ST-1 isolated modules are not mapped in the current process"
}

foreach ($pair in @(
    @($st2Controller, $remoteSt2),
    @($frameController, $remoteFrame),
    @($scanner, $remoteScanner)
)) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Push failed: $($pair[1])" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteSt2 $remoteFrame $remoteScanner'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Device chmod failed" }

$selftestLines = & $AdbPath -s $Device shell "su -c '$remoteFrame --selftest'" 2>&1
$selftestCode = $LASTEXITCODE
$selftestLines | Out-Host
if ($selftestCode -ne 0 -or
    ($selftestLines -join "`n") -notmatch 'FRAME_THREAD_PROBE_V1_SELFTEST passed=1 dr7=0x10001') {
    throw "FrameThread controller device selftest failed"
}

$st2Lines = & $AdbPath -s $Device shell "su -c '$remoteSt2 $gamePid $remoteBootstrap $remotePayload'" 2>&1
$st2Code = $LASTEXITCODE
$st2Lines | Out-Host
$st2Text = $st2Lines -join "`n"
if ($st2Code -ne 0 -or
    $st2Text -notmatch "SAME_THREAD_PROBE_V1_RESULT passed=1 pid=$gamePid tid=(\d+) status=1 trampoline=0x([0-9a-fA-F]+) returned_tid=\1 alive=1 tracer_pid=0") {
    throw "Same-process ST-2 revalidation failed"
}
$trampolineHex = $matches[2].ToLowerInvariant()

$afterSt2Pid = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
$tracerAfterSt2 = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($afterSt2Pid -ne $gamePidText -or
    ($tracerAfterSt2 -ne "TracerPid:`t0" -and $tracerAfterSt2 -ne "TracerPid: 0")) {
    throw "Process changed or tracer leaked after ST-2 revalidation"
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
$baseHex = $bases[0].ToString("x")

$scanLines = & $AdbPath -s $Device shell "su -c '$remoteScanner $gamePid $baseHex 0'" 2>&1
$scanCode = $LASTEXITCODE
$scanLines | Out-Host
$scanText = $scanLines -join "`n"
if ($scanCode -ne 0) { throw "Read-only command-owner scan failed" }
$ownerMatches = [regex]::Matches(
    $scanText,
    'ACTION07_COMMAND_PATH owner_adjustment=(-?\d+) owner=0x([0-9a-fA-F]+).*?direct_mode=(\d+)')
$owners = @($ownerMatches | ForEach-Object { $_.Groups[2].Value.ToLowerInvariant() } | Sort-Object -Unique)
if ($ownerMatches.Count -lt 1 -or $owners.Count -ne 1) {
    throw "Exactly one action-0x07 command owner was not resolved; ensure a naturally running race"
}
foreach ($match in $ownerMatches) {
    if ($match.Groups[1].Value -ne '-6056' -or
        $match.Groups[3].Value -ne '0') {
        throw "Command-owner adjustment/direct-mode precondition failed"
    }
}
$ownerHex = $owners[0]

$ack = "I_ACCEPT_FRAME_THREAD_ZERO_SIDE_EFFECT_PROBE_V1"
$frameLines = & $AdbPath -s $Device shell "su -c '$remoteFrame $gamePid $trampolineHex $ownerHex $TimeoutMs 0 $ack'" 2>&1
$frameCode = $LASTEXITCODE
$frameLines | Out-Host
$frameText = $frameLines -join "`n"
if ($frameCode -ne 0 -or
    $frameText -notmatch "FRAME_THREAD_PROBE_V1_RESULT passed=1 pid=$gamePid tid=(\d+) hits=(\d+) accepted=1 host_tid=\1 host_ok=1 guest_tid=\1 guest_ok=1 unexpected_stops=0 alive=1 tracer_pid=0 activations=0") {
    throw "ST-3 FrameThread zero-side-effect probe failed closed"
}

$finalPid = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
$tracerFinal = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$gamePid/status'" 2>$null) -join '').Trim()
if ($finalPid -ne $gamePidText -or
    ($tracerFinal -ne "TracerPid:`t0" -and $tracerFinal -ne "TracerPid: 0")) {
    throw "ST-3 cleanup verification failed"
}
Write-Output "ST3_FRAME_THREAD_PROBE_V1_PASSED pid=$gamePid owner=0x$ownerHex trampoline=0x$trampolineHex activations=0 TracerPid=0"
