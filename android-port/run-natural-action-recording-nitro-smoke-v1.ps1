param(
    [ValidateSet("OfflineValidate", "ExecutePreparedPaused")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [switch]$AcknowledgeRacePausedNitroIdleUsable,
    [switch]$AcknowledgeSingleEscResume,
    [switch]$AcknowledgeExactlyOneManualSpace,
    [switch]$AcknowledgeOneServiceVptrSwap,
    [switch]$AcknowledgeCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$candidate = Join-Path $root "build\natural-action-recording-nitro-smoke-v1\a9tas_natural_action_recording_nitro_smoke_v1_review_only"
$payload = Join-Path $root "build\natural-action-recording-payload-v1\liba9tas_natural_action_recording_v1_review_only.so"
$bootstrap = Join-Path $root "build\natural-action-recording-live-v1\liba9tas_bootstrap_natural_action_recording_v1.so"
$receipt = Join-Path $root "build\natural-action-recording-runner-v1\prepared-process-v1.json"
$candidatePin = "5a63b7e0c20b64c4995f4d9f656c16420b5162116dda7046fa2cced552a49ab3"
$payloadPin = "ad3ae03543b7489102adb53dba320296e93b034b12fe18bcf216969bc123c527"
$bootstrapPin = "44757629fcabc94517a05ec84fb2161d02bdaf424deeda9c84f469186d9b4d97"
$remoteCandidate = "/data/local/tmp/a9tas_natural_action_recording_nitro_smoke_v1"
$ack = "I_ACCEPT_ONE_PASSIVE_NITRO_RECORDING_SMOKE_V1"

function Get-Sha([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    $text = ((& $AdbPath @Arguments 2>&1) -join "`n").Trim()
    if ($LASTEXITCODE -ne 0) { throw "ADB command failed: $text" }
    $text
}
function Get-GamePid {
    $text = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
    if ($text -match '^\d+$') { return [int]$text }
    return 0
}
function Get-StartTicks([int]$GamePid) {
    $stat = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed process stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') { throw "Process start time unavailable" }
    [string]$fields[19]
}
function Get-TracerPid([int]$GamePid) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($line -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid" }
    [int]$matches[1]
}
function Get-GameBaseHex([int]$GamePid) {
    $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/maps'")
    $rawBases = @(foreach ($line in ($maps -split "`n")) {
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+.*libAsphalt9\.so' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) { $matches[1].ToLowerInvariant() }
    })
    $bases = @($rawBases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
    [string]$bases[0]
}
function Force-StopGame {
    & $AdbPath -s $Device shell "am force-stop $package" | Out-Null
}

foreach ($pair in @(@($candidate, $candidatePin), @($payload, $payloadPin), @($bootstrap, $bootstrapPin))) {
    if (-not (Test-Path -LiteralPath $pair[0] -PathType Leaf) -or (Get-Sha $pair[0]) -ne $pair[1]) {
        throw "Pinned local artifact mismatch: $($pair[0])"
    }
}
if ($Mode -eq "OfflineValidate") {
    Write-Output "NITRO_RECORDING_SMOKE_RUNNER_OFFLINE passed=1 deployed=0 device_access=0 hardware_breakpoints=0"
    return
}
foreach ($gate in @(
    @($AcknowledgeRacePausedNitroIdleUsable, "race paused with Nitro idle and usable"),
    @($AcknowledgeSingleEscResume, "one ESC resume"),
    @($AcknowledgeExactlyOneManualSpace, "exactly one manual Space input"),
    @($AcknowledgeOneServiceVptrSwap, "one service vptr install and restore"),
    @($AcknowledgeCrashRisk, "NativeBridge crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB unavailable" }
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared-process receipt missing" }
$prepared = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
$startTicks = if ($gamePid -gt 0) { Get-StartTicks $gamePid } else { '' }
if ($prepared.version -ne 1 -or $prepared.semantics -ne "natural_action_recording_source" -or
    $prepared.device -ne $Device -or [int]$prepared.pid -ne $gamePid -or
    [string]$prepared.start_time -ne $startTicks -or
    $prepared.payload_sha256 -ne $payloadPin -or $prepared.bootstrap_sha256 -ne $bootstrapPin) {
    throw "Prepared process identity mismatch"
}
if ((Get-TracerPid $gamePid) -ne 0) { throw "Game is already traced" }
$baseHex = Get-GameBaseHex $gamePid
& $AdbPath -s $Device push $candidate $remoteCandidate | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Candidate push failed" }
Invoke-AdbText @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteCandidate'") | Out-Null
$remoteHash = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $remoteCandidate'")
if ($remoteHash -notmatch '^([0-9a-fA-F]{64})\s+' -or $matches[1].ToLowerInvariant() -ne $candidatePin) {
    throw "Remote candidate hash mismatch"
}
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$readyPath = "/data/local/tmp/a9tas_nitro_recording_smoke_ready_${gamePid}_$stamp"
$remoteCommand = "$remoteCandidate $gamePid $baseHex 0 30000 $readyPath $ack"
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
if ($null -ne $startInfo.ArgumentList) {
    foreach ($argument in @('-s', $Device, 'shell', "su -c '$remoteCommand'")) { $startInfo.ArgumentList.Add($argument) }
} else {
    $startInfo.Arguments = "-s $Device shell `"su -c '$remoteCommand'`""
}
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) { throw "Candidate launch failed" }
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$released = $false
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
        $probe = ((& $AdbPath -s $Device shell "su -c 'if [ -e $readyPath ]; then echo READY; fi'" 2>$null) -join '').Trim()
        if ($probe -eq 'READY') { $ready = $true; break }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) { throw "Smoke candidate did not reach frozen READY" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
    if ($readyText -notmatch '^NITRO_RECORDING_SMOKE_READY ' -or
        $readyText -notmatch 'service_vptr_swaps=1 hardware_breakpoints=0 host_resume_gate=marker_removal$') {
        throw "Smoke READY proof mismatch"
    }
    $esc = Start-Process -FilePath $AdbPath -ArgumentList @('-s', $Device, 'shell', 'input', 'keyevent', '111') -WindowStyle Hidden -PassThru
    Invoke-AdbText @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") | Out-Null
    $released = $true
    if (-not $esc.WaitForExit(10000) -or $esc.ExitCode -ne 0) { throw "Single ESC resume failed" }
    Write-Output "NITRO_RECORDING_SMOKE_INPUT_WINDOW_OPEN: 现在只按一次空格，不要双击，也不要按其他键。"
    if (-not $process.WaitForExit(45000)) { throw "Smoke candidate timed out" }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if ($stdout) { $stdout.TrimEnd() | Write-Host }
    if ($stderr) { $stderr.TrimEnd() | Write-Warning }
    if ($process.ExitCode -ne 0 -or $stdout -notmatch 'NITRO_RECORDING_SMOKE_DONE complete=1 ') {
        $safeRollback = $stdout -match 'restored=1 ' -and $stdout -match 'target_alive=1'
        if (-not $safeRollback) {
            Force-StopGame
            Remove-Item -LiteralPath $receipt -Force -ErrorAction SilentlyContinue
        }
        throw "Nitro recording smoke failed closed (safe_rollback=$([int]$safeRollback))"
    }
    if ((Get-GamePid) -ne $gamePid -or (Get-TracerPid $gamePid) -ne 0) { throw "Post-smoke process identity mismatch" }
    Write-Output "NITRO_RECORDING_SMOKE_LIVE_PASSED pid=$gamePid calls=1 restored=1 TracerPid=0 hardware_breakpoints=0"
} finally {
    & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" 2>$null | Out-Null
    if (-not $released -and -not $process.HasExited) { [void]$process.WaitForExit(7000) }
}
