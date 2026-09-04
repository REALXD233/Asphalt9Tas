# Guarded 360-frame source capture sharing the final-writer input-cycle zero.
# Default mode is offline-only. Live capture requires a separate authorization.

param(
    [ValidateSet("OfflineValidate", "ExecuteCapture")]
    [string]$Mode = "OfflineValidate",
    [ValidateSet(360)][int]$TargetFrames = 360,
    [int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$RecordingOutputPath = "",
    [string]$ReportOutputPath = "",
    [switch]$ExecuteExactlyOneAttempt,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgePreAttachFrozenAndSingleEscape,
    [switch]$Acknowledge360FixedDeltaWrites,
    [switch]$AcknowledgeReadOnlyControlAndPhysicsCapture,
    [switch]$AcknowledgeNoManualInput,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$binary = Join-Path $root "build\input-cycle-startline-source-v1\a9tas_input_cycle_startline_source_v1"
$buildScript = Join-Path $root "build-input-cycle-startline-source-v1.ps1"
$source = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$validator = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$policy = Join-Path $root "tools\test_input_cycle_startline_source_policy_v1.py"
$ack = "I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_BRAKE_CAPTURE_V1"
$remoteBinary = "/data/local/tmp/a9tas_input_cycle_startline_source_v1"
$pins = @{
    $binary = "24687de130f0f1df8d3b9d9fff883ab3ceff0a6391fd9191caf57c50247b5bd2"
    $buildScript = "d4cc77767f5e6c216c6295e1e407cc1fde248733aa2dfe34b476b519c81b61c4"
    $source = "7dcfe4fb0117c5048257b784fc7368d9591ae60381a85b5d37b09f3eb58178a3"
    $validator = "01a66fe65543d0a5882920aa3dde94251e75057ce1e11da5e5abea189f09fc22"
    $policy = "9c49d4c8eb0ccba52bf90528caf686c4cb3055f0ba2afc763bfc9aeafe284408"
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments) | Out-String).Trim()
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
function Get-TracerPid([int]$GamePid) {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($value -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid: $value" }
    return [int]$matches[1]
}
function Get-LibraryBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $candidates = @(foreach ($line in $maps) {
        if ($line -match 'libAsphalt9\.so' -and
            $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) { $matches[1].ToLowerInvariant() }
    })
    $bases = @($candidates | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base" }
    return [string]$bases[0]
}
function Assert-RemoteHash([string]$Remote, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Remote'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Remote"
    }
}

if ($TimeoutMs -lt 30000 -or $TimeoutMs -gt 180000) {
    throw "TimeoutMs must be 30000..180000"
}
foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Sha $path) -ne $pins[$path]) {
        throw "Reviewed input-cycle source artifact mismatch: $path"
    }
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "Input-cycle source policy failed" }
if ($Mode -eq "OfflineValidate") {
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0 -or (Get-Sha $binary) -ne $pins[$binary]) {
        throw "Input-cycle source build/hash validation failed"
    }
    Write-Output "INPUT_CYCLE_STARTLINE_SOURCE_OFFLINE_VALID frames=360 deployed=0 device_access=0"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneAttempt, "exactly one attempt"),
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, "Ancient Ruins + ZL1 countdown-3 paused"),
    @($AcknowledgePreAttachFrozenAndSingleEscape, "pre-attach freeze and one ESC"),
    @($Acknowledge360FixedDeltaWrites, "exactly 360 fixed-delta writes"),
    @($AcknowledgeReadOnlyControlAndPhysicsCapture, "read-only control/physics capture"),
    @($AcknowledgeNoManualInput, "no manual input after resume"),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, "ptrace stall, rollback and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}
$gamePid = Get-GamePid
if ($gamePid -le 0) { throw "Game PID unavailable" }
$startTicks = Get-StartTicks $gamePid
if ((Get-TracerPid $gamePid) -ne 0) { throw "Game is already traced" }
$baseHex = Get-LibraryBaseHex $gamePid
$readyPath = "/data/local/tmp/a9tas_input_cycle_source_ready_$gamePid"
if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -ne '1') {
    throw "Stale input-cycle source READY marker exists"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($RecordingOutputPath -eq "") {
    $RecordingOutputPath = Join-Path $root "evidence\a9tas_input_cycle_source_360f_$stamp.a9utk1"
}
if ($ReportOutputPath -eq "") {
    $ReportOutputPath = Join-Path $root "evidence\a9tas_input_cycle_source_360f_$stamp.a9usr2"
}
$RecordingOutputPath = [IO.Path]::GetFullPath($RecordingOutputPath)
$ReportOutputPath = [IO.Path]::GetFullPath($ReportOutputPath)
foreach ($path in @($RecordingOutputPath, $ReportOutputPath)) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists: $path" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
}
$remoteRecording = "/data/local/tmp/a9tas_input_cycle_source_${gamePid}_$stamp.a9utk1"
$remoteReport = "/data/local/tmp/a9tas_input_cycle_source_${gamePid}_$stamp.a9usr2"
Invoke-AdbChecked @('-s', $Device, 'push', $binary, $remoteBinary) "push source candidate" | Out-Null
Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteBinary'") "chmod source candidate" | Out-Null
Assert-RemoteHash $remoteBinary $pins[$binary]

$remoteCommand = "$remoteBinary $gamePid $baseHex $TimeoutMs $TargetFrames 16667 $remoteRecording $remoteReport $ack 0 0 0"
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
if (-not $candidate.Start()) { throw "Failed to launch input-cycle source candidate" }
$stdoutTask = $candidate.StandardOutput.ReadToEndAsync()
$stderrTask = $candidate.StandardError.ReadToEndAsync()

$ready = $false
$releaseCommitted = $false
$forcedTracerTermination = $false
$primaryFailure = ""
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline -and -not $candidate.HasExited) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -eq '0') {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw "Input-cycle source did not publish READY" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
    $identityMatch = [regex]::Match(
        $readyText,
        "^READY_ARMED_INPUT_CYCLE_SOURCE_V1 pid=$gamePid controller_pid=(\d+) "
    )
    $frozenProofMatch = [regex]::IsMatch(
        $readyText,
        'target_threads_attached=[1-9][0-9]* all_target_threads_frozen=1 ' +
        'thread_set_stable=1 freeze_passes=[2-4] gameplay_state_writes=0'
    )
    if (-not $identityMatch.Success -or -not $frozenProofMatch) {
        throw "Input-cycle source READY proof mismatch"
    }
    $controllerPid = [int]$identityMatch.Groups[1].Value
    if ((Get-TracerPid $gamePid) -ne $controllerPid) {
        throw "Armed tracer identity mismatch"
    }
    Write-Host "INPUT_CYCLE_SOURCE_HOST_READY pid=$gamePid frames=360 releasing_frozen_threads=1"
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") "remove source READY marker" | Out-Null
    $releaseCommitted = $true
    Write-Host "INPUT_CYCLE_SOURCE_HOST_RELEASED pid=$gamePid sending_single_ESC=1"
    Invoke-AdbChecked @('-s', $Device, 'shell', 'input keyevent 111') "single ESC" | Out-Null
    if (-not $candidate.WaitForExit($TimeoutMs + 15000)) {
        throw "Input-cycle source timed out; no retry"
    }
} catch {
    $primaryFailure = $_.Exception.Message
}

if ($primaryFailure -ne "" -and -not $candidate.HasExited) {
    # Before marker removal the candidate has a 20-second internal rollback.
    # After release, its capture deadline is bounded by TimeoutMs. Neither path
    # may become an unbounded host wait.
    $rollbackWaitMs = if ($releaseCommitted) { 5000 } else { 25000 }
    [void]$candidate.WaitForExit($rollbackWaitMs)
}
if ($primaryFailure -ne "" -and -not $candidate.HasExited) {
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
if ($primaryFailure -ne "") {
    & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null
}
if (-not $candidate.HasExited) {
    try { $candidate.Kill($true) } catch {}
    [void]$candidate.WaitForExit(5000)
}
if (-not $candidate.HasExited) {
    throw "$primaryFailure; candidate cleanup did not complete"
}
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if ($stdout) { Write-Host $stdout.TrimEnd() }
if ($stderr) { Write-Warning $stderr.TrimEnd() }
if ($primaryFailure -ne "") {
    $cleanupTracer = if ((Get-GamePid) -eq $gamePid) {
        Get-TracerPid $gamePid
    } else { -1 }
    if ($cleanupTracer -ne 0) {
        throw "$primaryFailure; cleanup TracerPid=$cleanupTracer; restart game process"
    }
    if ($forcedTracerTermination) {
        throw "$primaryFailure; forced tracer termination used; restart game process"
    }
    throw "$primaryFailure; rollback complete; no retry"
}
if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks) {
    throw "Game process changed during source capture"
}
if ((Get-TracerPid $gamePid) -ne 0) { throw "Source capture left a tracer attached" }
if ($candidate.ExitCode -ne 0) {
    throw "Input-cycle source failed closed (exit=$($candidate.ExitCode)); no retry"
}
Invoke-AdbChecked @('-s', $Device, 'shell',
    "su -c 'chmod 644 $remoteRecording $remoteReport'") "chmod source reports" | Out-Null
foreach ($pair in @(@($remoteRecording, $RecordingOutputPath), @($remoteReport, $ReportOutputPath))) {
    Invoke-AdbChecked @('-s', $Device, 'pull', $pair[0], $pair[1]) "pull source artifact" | Out-Null
}
python -B $validator --require-input-cycle-anchor `
    $ReportOutputPath $RecordingOutputPath
if ($LASTEXITCODE -ne 0) { throw "Input-cycle A9USR2/A9UTK1 validation failed" }
Write-Output "INPUT_CYCLE_STARTLINE_SOURCE_LIVE_PASSED frames=360 single_ESC=1 TracerPid=0"
Write-Output "Recording: $RecordingOutputPath"
Write-Output "Report: $ReportOutputPath"
