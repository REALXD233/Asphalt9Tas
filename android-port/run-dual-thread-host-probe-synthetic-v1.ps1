# Device-side synthetic integration test for DTA-0. This launches an isolated
# three-thread fixture and never attaches to the game process.
param(
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [ValidateRange(1, 20)]
    [int]$Iterations = 5,
    [switch]$OfflineValidateOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$controller = Join-Path $root "build\same-thread-probe-v1\a9tas_dual_thread_host_probe_controller_v1"
$controllerSource = Join-Path $root "src\dual_thread_host_probe_controller_v1.cpp"
$frameSource = Join-Path $root "src\same_thread_frame_probe_controller_v1.cpp"
$commonSource = Join-Path $root "src\same_thread_probe_controller_v1.cpp"
$injectorSource = Join-Path $root "src\injector.cpp"
$fixture = Join-Path $root "build\same-thread-probe-v1\a9tas_dual_thread_host_probe_fixture_v1"
$fixtureSource = Join-Path $root "src\dual_thread_host_probe_fixture_v1.cpp"
$buildScript = Join-Path $root "build-same-thread-probe-v1.ps1"
$pins = @{
    $controller = "8682bb3803874420689b78ab12b2459021dfe9f06740f9945444c9f9cf15b2b8"
    $controllerSource = "9b7d838ebcf3e5d36da9e4161d6047dc25d744e671378684c805b5fe34a31199"
    $frameSource = "a21623b828c087f71fbd880e9f92cee42e80be7b7104467fb91933911f0c5b75"
    $commonSource = "14ab2e90b905bc2cd5f3dfff8e054d7fbdc2762a970963c32fcf9bd5758f1cc7"
    $injectorSource = "bcc29f5f9470332abfcc87b1c8ae715a12633f79de93667c453c1097856c74b2"
    $fixture = "79714bdd5ce8d6183ae4cbdd885756117504abf60703411ca0d33ad431cdba4f"
    $fixtureSource = "4902f283ad9c1ec6f1404f6153f9e68aaf0e55f3db24cd17355175364a71cb54"
    $buildScript = "5a4d4db64c3da94378e80936635ccbd02d5676dd51807be79dbbd5d2c4a1325f"
}

foreach ($artifact in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing synthetic DTA-0 artifact: $artifact"
    }
    $actual = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $pins[$artifact]) {
        throw "Synthetic DTA-0 hash mismatch: $artifact"
    }
}

$readyPattern = 'DTA0_FIXTURE_V1_READY pid=(\d+) owner=0x([0-9a-fA-F]+) writer_tid=(\d+) writer_name=DTA0_Writer frame_name=FrameThread_0'
$resultPattern = 'DUAL_THREAD_HOST_PROBE_V1_RESULT passed=(\d+) pid=(\d+) writer_tid=(\d+) writer_name=(.*?) frame_tid=(\d+) hits=(\d+) writer_hit=(\d+) host_tid=(\d+) host_ok=(\d+) hold_ns=(\d+) unexpected_stops=(\d+) alive=(\d+) tracer_pid=(\d+) guest_calls=(\d+) activations=(\d+)'
if ($OfflineValidateOnly) {
    $ready = [regex]::Match('DTA0_FIXTURE_V1_READY pid=8497 owner=0x7ffff7e0d000 writer_tid=8499 writer_name=DTA0_Writer frame_name=FrameThread_0', $readyPattern)
    $result = [regex]::Match('DUAL_THREAD_HOST_PROBE_V1_RESULT passed=1 pid=8497 writer_tid=8499 writer_name=DTA0 Writer frame_tid=8500 hits=1 writer_hit=1 host_tid=8500 host_ok=1 hold_ns=11569661 unexpected_stops=0 alive=1 tracer_pid=0 guest_calls=0 activations=0', $resultPattern)
    if (-not $ready.Success -or -not $result.Success -or
        $ready.Groups[1].Value -ne $result.Groups[2].Value -or
        $ready.Groups[3].Value -ne $result.Groups[3].Value -or
        $result.Groups[5].Value -ne $result.Groups[8].Value -or
        $result.Groups[11].Value -ne '0' -or
        $result.Groups[13].Value -ne '0' -or
        $result.Groups[14].Value -ne '0' -or
        $result.Groups[15].Value -ne '0') {
        throw "Synthetic DTA-0 parser selftest failed"
    }
    Write-Output "DUAL_THREAD_HOST_PROBE_SYNTHETIC_V1_OFFLINE_OK"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB not found: $AdbPath"
}
$remoteController = '/data/local/tmp/a9tas_dual_thread_host_probe_controller_v1'
$remoteFixture = '/data/local/tmp/a9tas_dual_thread_host_probe_fixture_v1'
foreach ($pair in @(@($controller, $remoteController), @($fixture, $remoteFixture))) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Device push failed: $($pair[1])" }
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteController $remoteFixture'" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Device chmod failed" }

$package = 'com.aligames.kuang.kybc.aligames'
$gamePidBefore = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
$stamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$passes = 0
for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
    $fixtureProcessId = ''
    $logRemote = "/data/local/tmp/a9tas_dta0_fixture_${stamp}_${iteration}.log"
    try {
        $fixtureProcessId = ((& $AdbPath -s $Device shell "su -c '$remoteFixture > $logRemote 2>&1 & echo `$!'" 2>&1) -join '').Trim()
        if ($fixtureProcessId -notmatch '^\d+$') {
            throw "Fixture launch failed: $fixtureProcessId"
        }
        $fixtureText = ''
        for ($attempt = 0; $attempt -lt 50; ++$attempt) {
            $fixtureText = ((& $AdbPath -s $Device shell "su -c 'cat $logRemote'" 2>$null) -join "`n").Trim()
            if ($fixtureText -match $readyPattern) { break }
            Start-Sleep -Milliseconds 100
        }
        $ready = [regex]::Match($fixtureText, $readyPattern)
        if (-not $ready.Success -or $ready.Groups[1].Value -ne $fixtureProcessId) {
            throw "Fixture readiness validation failed"
        }
        $ownerHex = $ready.Groups[2].Value
        $writerTid = $ready.Groups[3].Value
        $controllerLines = & $AdbPath -s $Device shell "su -c '$remoteController $fixtureProcessId $ownerHex $writerTid 5000 0 I_ACCEPT_DUAL_THREAD_HOST_GETTID_PROBE_V1'" 2>&1
        $controllerCode = $LASTEXITCODE
        $controllerLines | Out-Host
        $result = [regex]::Match(($controllerLines -join "`n"), $resultPattern)
        if ($controllerCode -ne 0 -or -not $result.Success -or
            $result.Groups[1].Value -ne '1' -or
            $result.Groups[2].Value -ne $fixtureProcessId -or
            $result.Groups[3].Value -ne $writerTid -or
            $result.Groups[5].Value -ne $result.Groups[8].Value -or
            $result.Groups[7].Value -ne '1' -or
            $result.Groups[9].Value -ne '1' -or
            $result.Groups[11].Value -ne '0' -or
            $result.Groups[12].Value -ne '1' -or
            $result.Groups[13].Value -ne '0' -or
            $result.Groups[14].Value -ne '0' -or
            $result.Groups[15].Value -ne '0') {
            throw "Synthetic DTA-0 iteration $iteration failed"
        }
        $fixtureTracer = ((& $AdbPath -s $Device shell "su -c 'grep ^TracerPid: /proc/$fixtureProcessId/status'" 2>$null) -join '').Trim()
        if ($fixtureTracer -ne "TracerPid:`t0" -and $fixtureTracer -ne 'TracerPid: 0') {
            throw "Synthetic fixture tracer leaked"
        }
        ++$passes
    } finally {
        if ($fixtureProcessId -match '^\d+$') {
            $fixtureCommand = ((& $AdbPath -s $Device shell "su -c 'cat /proc/$fixtureProcessId/cmdline'" 2>$null) -join '')
            if ($fixtureCommand -match 'a9tas_dual_thread_host_probe_fixture_v1') {
                & $AdbPath -s $Device shell "su -c 'kill $fixtureProcessId'" 2>$null | Out-Null
            }
        }
        & $AdbPath -s $Device shell "su -c 'rm -f $logRemote'" 2>$null | Out-Null
    }
}

$gamePidAfter = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
if ($gamePidBefore -ne $gamePidAfter) { throw "Game PID changed during isolated selftest" }
if ($gamePidAfter -match '^\d+$') {
    $gameTracer = ((& $AdbPath -s $Device shell "su -c 'grep ^TracerPid: /proc/$gamePidAfter/status'" 2>$null) -join '').Trim()
    if ($gameTracer -ne "TracerPid:`t0" -and $gameTracer -ne 'TracerPid: 0') {
        throw "Game tracer changed during isolated selftest"
    }
}
Write-Output "DUAL_THREAD_HOST_PROBE_SYNTHETIC_V1_PASSED iterations=$passes game_pid=$gamePidAfter game_TracerPid=0"
