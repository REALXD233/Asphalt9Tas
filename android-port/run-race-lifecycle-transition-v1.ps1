# Guarded one-shot observer for the authoritative race lifecycle 2 -> 3 edge.
# Default mode is offline-only and never contacts ADB.

param(
    [ValidateSet("OfflineValidate", "ExecuteTransition")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [ValidateRange(5000, 60000)][int]$TimeoutMs = 30000,
    [string]$ObjectHex = "",
    [string]$StateAddressHex = "",
    [string]$OutputDirectory = "",
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeCurrentReadOnlyLifecycleAddresses,
    [switch]$AcknowledgeSingleEscBeforeTracerRelease,
    [switch]$AcknowledgePtraceDebugRegistersOnly,
    [switch]$AcknowledgeStallCleanupAndCrashRisk,
    [switch]$ExecuteExactlyOneAttempt
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$binary = Join-Path $root "build\race-lifecycle-transition-v1\a9tas_race_lifecycle_transition_v1_review_only"
$source = Join-Path $root "src\hwbp_race_lifecycle_transition_v1.cpp"
$scheduler = Join-Path $root "src\hwbp_scheduler_observer_v1.cpp"
$resolverHeader = Join-Path $root "src\race_lifecycle_object_resolver_v1.h"
$observerPolicy = Join-Path $root "tools\test_race_lifecycle_transition_policy_v1.py"
$buildScript = Join-Path $root "build-race-lifecycle-transition-v1.ps1"
$remoteBinary = "/data/local/tmp/a9tas_race_lifecycle_transition_v1"
$ack = "I_ACCEPT_ONE_RACE_LIFECYCLE_2_TO_3_WATCH_V1"

$pins = @{
    $binary = "d43bd49d9d0dce11c286027592b800e72dbf5149c72de01ed07a1a84afd13cf6"
    $source = "3bbef4bed2eeffa2a88d3965369229280bcd5481bc98754baded5feb22e09779"
    $scheduler = "c4e4ddab9a66bc0f5a1f60e915ed8160cdf04a14a028a984997c4c2c063a70b6"
    $resolverHeader = "a2d1c55d3cf24e4f3afea35d1aa5983e62e2ee29a08d199d7b293db4de0b472b"
    $observerPolicy = "311631496cb9a76af325e49c6bce6a56f83a11fba75258fb531e19e04d818e96"
    $buildScript = "6fae726776b95881c83fed96351b5d7b1bcf8eabe6d588020592b240be206307"
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
function Start-SingleEscInjection {
    $process = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'input', 'keyevent', '111') `
        -WindowStyle Hidden -PassThru
    if ($null -eq $process) { throw "Failed to start single ESC injection" }
    return $process
}
function Complete-SingleEscInjection([Diagnostics.Process]$Process) {
    if (-not $Process.WaitForExit(10000)) {
        try { Stop-Process -Id $Process.Id -Force } catch {}
        throw "Single ESC injection timed out after target release"
    }
    if ($Process.ExitCode -ne 0) {
        throw "Single ESC injection failed after target release (exit=$($Process.ExitCode))"
    }
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
function Normalize-HexAddress([string]$Value, [string]$Label) {
    $normalized = $Value.Trim()
    if ($normalized.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) {
        $normalized = $normalized.Substring(2)
    }
    if ($normalized -notmatch '^[0-9a-fA-F]{8,16}$') {
        throw "$Label must be an 8..16 digit hexadecimal address"
    }
    return $normalized.ToLowerInvariant()
}

foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing reviewed lifecycle transition input: $path"
    }
    if ((Get-Sha $path) -ne $pins[$path]) {
        throw "Lifecycle transition pin mismatch: $path"
    }
}
python -B $observerPolicy $binary
if ($LASTEXITCODE -ne 0) { throw "Lifecycle transition observer policy failed" }

if ($Mode -eq "OfflineValidate") {
    Write-Output "RACE_LIFECYCLE_TRANSITION_RUNNER_OFFLINE passed=1 deployed=0 device_access=0"
    return
}

foreach ($gate in @(
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, "Ancient Ruins + ZL1 countdown-3 paused"),
    @($AcknowledgeCurrentReadOnlyLifecycleAddresses, "current read-only lifecycle object and state addresses"),
    @($AcknowledgeSingleEscBeforeTracerRelease, "exactly one ESC while target threads remain frozen"),
    @($AcknowledgePtraceDebugRegistersOnly, "ptrace debug-register-only lifecycle watch"),
    @($AcknowledgeStallCleanupAndCrashRisk, "stall, bounded cleanup and crash risk"),
    @($ExecuteExactlyOneAttempt, "exactly one attempt")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }

$object = Normalize-HexAddress $ObjectHex "ObjectHex"
$stateAddress = Normalize-HexAddress $StateAddressHex "StateAddressHex"
$objectValue = [Convert]::ToUInt64($object, 16)
$stateValue = [Convert]::ToUInt64($stateAddress, 16)
if ($objectValue -gt ([UInt64]::MaxValue - 0x2d8) -or
    $stateValue -ne ($objectValue + 0x2d8)) {
    throw "StateAddressHex must equal ObjectHex + 0x2D8"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}
$gamePid = Get-GamePid
if ($gamePid -le 0) { throw "Game process unavailable" }
$startTicks = Get-StartTicks $gamePid
if ((Get-TracerPid $gamePid) -ne 0) { throw "Game already has a tracer" }
$base = Get-LibraryBaseHex $gamePid
$readyPath = "/data/local/tmp/a9tas_race_lifecycle_transition_ready_$gamePid"
if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -ne '1') {
    throw "Stale lifecycle transition READY marker exists"
}

Invoke-AdbChecked @('-s', $Device, 'push', $binary, $remoteBinary) `
    "push lifecycle transition observer" | Out-Null
Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteBinary'") `
    "chmod lifecycle transition observer" | Out-Null
Assert-RemoteHash $remoteBinary $pins[$binary]

$remoteCommand = "$remoteBinary $gamePid $base $TimeoutMs $startTicks $object $stateAddress $readyPath $ack"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in @('-s', $Device, 'shell', "su -c '$remoteCommand'")) {
    $startInfo.ArgumentList.Add($argument)
}
$candidate = [Diagnostics.Process]::new()
$candidate.StartInfo = $startInfo
if (-not $candidate.Start()) { throw "Failed to launch lifecycle transition observer" }
$stdoutTask = $candidate.StandardOutput.ReadToEndAsync()
$stderrTask = $candidate.StandardError.ReadToEndAsync()

$ready = $false
$escQueued = $false
$escProcess = $null
$releaseCommitted = $false
$forcedTracerTermination = $false
$primaryFailure = ""
try {
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $readyDeadline -and -not $candidate.HasExited) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -eq '0') {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) {
        $exitDetail = if ($candidate.HasExited) {
            " candidate_exit=$($candidate.ExitCode)"
        } else { " candidate_still_running=1" }
        throw "Lifecycle transition observer did not publish READY;$exitDetail"
    }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
    $escapedObject = [regex]::Escape($object)
    $escapedState = [regex]::Escape($stateAddress)
    $readyPattern = "^READY_RACE_LIFECYCLE_V1 pid=$gamePid start_ticks=$startTicks " +
        "controller_pid=(\d+) object=0x$escapedObject state_address=0x$escapedState " +
        "state=2 attached_threads=([1-9][0-9]*) all_target_threads_frozen=1 " +
        "thread_set_stable=1 freeze_passes=([1-4]) gameplay_writes=0$"
    $readyMatch = [regex]::Match($readyText, $readyPattern)
    if (-not $readyMatch.Success) { throw "Lifecycle transition READY proof mismatch: $readyText" }
    $controllerPid = [int]$readyMatch.Groups[1].Value
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
        (Get-TracerPid $gamePid) -ne $controllerPid) {
        throw "Armed lifecycle transition identity mismatch"
    }
    Write-Host "RACE_LIFECYCLE_TRANSITION_HOST_READY pid=$gamePid state=2 sending_single_ESC_while_frozen=1"
    # Android's synchronous `input keyevent` waits for the frozen app to
    # acknowledge delivery.  Start it first, release immediately, then wait;
    # otherwise the host and target can deadlock until the input command drops.
    $escProcess = Start-SingleEscInjection
    $escQueued = $true
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") `
        "release lifecycle transition observer" | Out-Null
    $releaseCommitted = $true
    Complete-SingleEscInjection $escProcess
    Write-Host "RACE_LIFECYCLE_TRANSITION_HOST_RELEASED pid=$gamePid ESC_before_release=1"
    if (-not $candidate.WaitForExit($TimeoutMs + 15000)) {
        throw "Lifecycle transition observer timed out; no retry"
    }
} catch {
    $primaryFailure = $_.Exception.Message
}

if ($primaryFailure -ne "" -and -not $candidate.HasExited) {
    try {
        & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
        $releaseCommitted = $true
    } catch {}
    [void]$candidate.WaitForExit($TimeoutMs + 5000)
}
if (-not $candidate.HasExited) {
    $liveTracer = 0
    try {
        if ((Get-GamePid) -eq $gamePid) { $liveTracer = Get-TracerPid $gamePid }
    } catch { $liveTracer = 0 }
    if ($liveTracer -gt 0) {
        $forcedTracerTermination = $true
        & $AdbPath -s $Device shell "su -c 'kill -TERM $liveTracer'" | Out-Null
        [void]$candidate.WaitForExit(5000)
    }
}
if (-not $candidate.HasExited) {
    try { $candidate.Kill($true) } catch {}
    [void]$candidate.WaitForExit(5000)
}
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if ($stdout) { Write-Host $stdout.TrimEnd() }
if ($stderr) { Write-Warning $stderr.TrimEnd() }
& $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null

$postPid = Get-GamePid
$postTracer = if ($postPid -eq $gamePid) { Get-TracerPid $gamePid } else { -1 }
if ($primaryFailure -ne "") {
    if ($forcedTracerTermination -or $postTracer -ne 0) {
        throw "$primaryFailure; forced or incomplete tracer cleanup; restart game process; no retry"
    }
    throw "$primaryFailure; bounded rollback complete; no retry"
}
if (-not $escQueued -or -not $releaseCommitted) {
    throw "Lifecycle transition host ordering proof incomplete"
}
if ($postPid -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks) {
    throw "Game process changed during lifecycle transition observation"
}
if ($postTracer -ne 0) { throw "Lifecycle transition observer left a tracer attached" }
if ($candidate.ExitCode -ne 0) {
    throw "Lifecycle transition observer failed closed (exit=$($candidate.ExitCode)); no retry"
}
$donePattern = 'RACE_LIFECYCLE_TRANSITION_DONE accepted=1 before=2 after=3 final=3 events=1 ' +
    'event_tid=[1-9][0-9]* event_ns=[1-9][0-9]* event_rip=0x[0-9a-f]+ ' +
    'initial_threads=[1-9][0-9]* additions=[0-9]+ exited=[0-9]+ detached=[1-9][0-9]* ' +
    'read_errors=0 ptrace_errors=0 semantic_errors=0 unexpected_stops=0 clean_detach=1 ' +
    'target_memory_write_attempts=0 gameplay_writes=0'
if ($stdout -notmatch $donePattern) {
    throw "Lifecycle transition completion proof mismatch; no retry"
}

if ($OutputDirectory -eq "") { $OutputDirectory = Join-Path $root "evidence" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$report = Join-Path $OutputDirectory "race_lifecycle_transition_${stamp}.txt"
$reportLines = @(
    "RACE_LIFECYCLE_TRANSITION_HOST_PASS pid=$gamePid start_ticks=$startTicks object=0x$object state_address=0x$stateAddress single_ESC_before_release=1 TracerPid=0 gameplay_writes=0",
    $stdout.Trim()
)
$reportLines | Set-Content -LiteralPath $report -Encoding ascii
$hash = Get-Sha $report
Write-Output "RACE_LIFECYCLE_TRANSITION_LIVE_PASS report=$report sha256=$hash"
