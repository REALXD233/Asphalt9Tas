param(
    [string]$Adb = "D:\leidian\LDPlayer9\adb.exe",
    [string]$Serial = "emulator-5554",
    [string]$Package = "com.aligames.kuang.kybc.aligames"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$idlePayload = Join-Path $projectRoot "build\staging\liba9tas_payload_idle.so"
$expectedHash = "79d7cf6cc82d56fa8663499d8e8d718a4a5741fc23cebf95ddbc3f4ccd2d298d"
$remotePayload = "/data/local/tmp/liba9tas_payload.so"

if (-not (Test-Path -LiteralPath $Adb)) {
    throw "ADB was not found at $Adb"
}
if (-not (Test-Path -LiteralPath $idlePayload)) {
    throw "The verified idle payload is missing: $idlePayload"
}

$localHash = (Get-FileHash -LiteralPath $idlePayload -Algorithm SHA256).Hash.ToLowerInvariant()
if ($localHash -ne $expectedHash) {
    throw "Idle payload hash mismatch. Expected $expectedHash, got $localHash"
}

$deviceState = (& $Adb -s $Serial get-state 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or $deviceState -ne "device") {
    throw "ADB device $Serial is not connected and ready"
}

$gamePidOutput = & $Adb -s $Serial shell "pidof $Package"
$gamePid = if ($null -eq $gamePidOutput) { "" } else { ($gamePidOutput | Out-String).Trim() }
if ($gamePid) {
    throw "The game is running (PID $gamePid). Stop it before restoring the baseline."
}

& $Adb -s $Serial push $idlePayload $remotePayload | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to push the idle payload"
}

$cleanup = @(
    "/data/local/tmp/a9tas-enable-physics-probe",
    "/data/local/tmp/a9tas-record-ctl",
    "/data/local/tmp/a9tas-inject-seq",
    "/data/local/tmp/a9tas-sample-ctl",
    "/data/local/tmp/a9tas-inject-key",
    "/data/local/tmp/a9tas-dev-sample-ctl",
    "/data/local/tmp/a9tas-interval-ctl",
    "/data/local/tmp/a9tas-race-ctl",
    "/data/local/tmp/a9tas-enable-control-values-probe",
    "/data/local/tmp/a9tas-enable-action-subscriber-probe",
    "/data/local/tmp/a9tas-enable-action-subscriber-probe-v3",
    "/data/local/tmp/a9tas-enable-input-receiver-probe",
    "/data/local/tmp/a9tas-enable-keyboard-action-probe",
    "/data/local/tmp/a9tas-enable-second-hop-probe",
    "/data/local/tmp/a9tas-enable-dual-hop-probe",
    "/data/local/tmp/a9tas-enable-gateway-probe"
)
$permissionCommand = "su -c 'chmod 644 $remotePayload'"
& $Adb -s $Serial shell $permissionCommand
if ($LASTEXITCODE -ne 0) {
    throw "Failed to set payload permissions"
}
foreach ($controlPath in $cleanup) {
    $removeCommand = "su -c 'rm -f $controlPath'"
    & $Adb -s $Serial shell $removeCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to remove experimental control: $controlPath"
    }
}

$remoteHashLine = (& $Adb -s $Serial shell "sha256sum '$remotePayload'").Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Could not verify the remote payload"
}
$remoteHash = ($remoteHashLine -split "\s+")[0].ToLowerInvariant()
if ($remoteHash -ne $expectedHash) {
    throw "Remote payload hash mismatch. Expected $expectedHash, got $remoteHash"
}

# Post-cleanup verification: no a9tas-enable-* marker may remain.
$remainingMarkers = (& $Adb -s $Serial shell "su -c 'ls /data/local/tmp/a9tas-enable-* 2>/dev/null'")
$remainingMarkerLines = @($remainingMarkers | Where-Object { $_ -and $_.Trim() })
if ($remainingMarkerLines.Count -gt 0) {
    throw "Marker cleanup incomplete. Remaining: $($remainingMarkerLines -join '; ')"
}

# Post-cleanup verification: no injector/debugger processes may remain.
$toolProcesses = (& $Adb -s $Serial shell "ps -A | grep -E 'a9tas_injector|gdbserver|lldb-server'")
$toolLines = @($toolProcesses | Where-Object { $_ -and $_.Trim() })
if ($toolLines.Count -gt 0) {
    throw "Tool process still running: $($toolLines -join '; ')"
}

# Post-cleanup verification: wrap property must be empty.
$wrapProperty = (& $Adb -s $Serial shell "getprop wrap.com.aligames.kuang.kybc.aligames")
if ($LASTEXITCODE -ne 0 -or ($wrapProperty | Out-String).Trim() -ne "") {
    throw "wrap property is not empty: '$($wrapProperty | Out-String)'"
}

Write-Output "Safe idle baseline restored. Payload SHA256: $remoteHash"
Write-Output "Marker/tool/wrap post-checks passed. The game was not started and no injector was run."
