param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ProbePreparedProcess")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$TimeoutMs = 1500,
    [int]$ReceiptTtlSeconds = 1800,
    [int]$PostProbeObservationMs = 3000,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgeNaturalRunningRace,
    [switch]$AcknowledgeAttachReadDetachOnly,
    [switch]$AcknowledgeNoHardwareWatchpointsOrDebugWrites,
    [switch]$AcknowledgeNoPayloadVptrOrGameWrites,
    [switch]$AcknowledgePtraceStalls,
    [switch]$AcknowledgeExitKillMayTerminateFreshProcess,
    [switch]$AcknowledgeFailureForceStopsFreshProcess
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\fc3-entry-stability-v1"
$controller = Join-Path $root "build\fc3-phase-map-candidate-v1\a9tas_fc3_phase_map_controller_v1"
$validator = Join-Path $root "tools\validate_fc3_entry_stability_v1.py"
$receipt = Join-Path $outDir "fc3-entry-stability-prepared-v1.json"
$remoteController = "/data/local/tmp/a9tas_fc3_entry_stability_controller_v1"
$ack = "I_ACCEPT_FC3_ENTRY_STABILITY_ATTACH_READ_DETACH_V1"
$controllerHash = "3a2a3a0077638ef146726b4f2e727e5de9d6092619d33d925c6d4d6686b3e999"
$validatorHash = "f641a4ecd4bdadd0ab581240e7f705900af4a57b733919730e33ae80d479cb76"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-AdbChecked([string[]]$Arguments, [string]$Description) {
    $lines = & $AdbPath @Arguments 2>&1
    $code = $LASTEXITCODE
    $lines | Out-Host
    if ($code -ne 0) { throw "$Description failed (exit=$code)" }
    return $lines
}

function Get-GamePid {
    $value = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
    if ($value -notmatch '^\d+$') { return 0 }
    return [int]$value
}

function Get-StartTime([int]$TargetPid) {
    $text = ((& $AdbPath -s $Device shell "su -c 'cat /proc/$TargetPid/stat'" 2>$null) -join '').Trim()
    if ($text -notmatch '^\d+\s+\(.*\)\s+\S\s+(.+)$') { throw "Process stat read failed" }
    $fields = @($matches[1] -split '\s+')
    if ($fields.Count -lt 19 -or $fields[18] -notmatch '^\d+$') { throw "Process start time invalid" }
    return $fields[18]
}

function Get-BootId {
    $value = ((& $AdbPath -s $Device shell 'cat /proc/sys/kernel/random/boot_id' 2>$null) -join '').Trim().ToLowerInvariant()
    if ($value -notmatch '^[0-9a-f-]{36}$') { throw "Device boot ID invalid" }
    return $value
}

function Get-Base([int]$TargetPid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$TargetPid/maps'"
    $bases = @($maps | ForEach-Object {
        if ($_ -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+0+\s+.*libAsphalt9\.so') { $matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
    return $bases[0]
}

function Assert-CleanTracer([int]$TargetPid) {
    $line = ((& $AdbPath -s $Device shell "su -c 'grep `"^TracerPid:`" /proc/$TargetPid/status'" 2>$null) -join '').Trim()
    if ($line -ne "TracerPid:`t0" -and $line -ne "TracerPid: 0") { throw "Active tracer detected: $line" }
}

function Get-RemoteHash([string]$Path) {
    $line = ((& $AdbPath -s $Device shell "su -c 'sha256sum $Path'" 2>$null) -join '').Trim()
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+') { throw "Remote hash failed: $Path" }
    return $matches[1].ToLowerInvariant()
}

function Stop-FreshPackage {
    & $AdbPath -s $Device shell "am force-stop $package" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Package force-stop failed" }
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Package remained alive after force-stop" }
}

foreach ($artifact in @(@($controller, $controllerHash), @($validator, $validatorHash))) {
    if (-not (Test-Path -LiteralPath $artifact[0] -PathType Leaf) -or
        (Get-Sha256 $artifact[0]) -ne $artifact[1]) { throw "Pinned artifact mismatch: $($artifact[0])" }
}
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "Entry validator selftest failed" }
if ($TimeoutMs -lt 250 -or $TimeoutMs -gt 3000) { throw "TimeoutMs must be 250..3000" }
if ($ReceiptTtlSeconds -lt 60 -or $ReceiptTtlSeconds -gt 3600) { throw "ReceiptTtlSeconds must be 60..3600" }
if ($PostProbeObservationMs -lt 1000 -or $PostProbeObservationMs -gt 10000) { throw "PostProbeObservationMs must be 1000..10000" }

if ($Mode -eq "OfflineValidateOnly") {
    Write-Output "FC3_ENTRY_RUNNER_OFFLINE_OK device_access=0 restart=0 attached=0 debug_writes=0 game_writes=0"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

if ($Mode -eq "PrepareFreshProcess") {
    if (-not $AcknowledgeFreshGameProcessRestart) { throw "Must acknowledge fresh game process restart" }
    $prepared = $false
    try {
        Stop-FreshPackage
        if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
        Invoke-AdbChecked @('-s', $Device, 'push', $controller, $remoteController) "push entry controller" | Out-Null
        Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 0700 $remoteController'") "chmod entry controller" | Out-Null
        if ((Get-RemoteHash $remoteController) -ne $controllerHash) { throw "Remote controller hash mismatch" }
        Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") "start fresh game process" | Out-Null
        $gamePid = 0
        $mapped = $false
        for ($attempt = 0; $attempt -lt 450; $attempt++) {
            $gamePid = Get-GamePid
            if ($gamePid -gt 0) {
                $mapped = ((& $AdbPath -s $Device shell "su -c 'grep libAsphalt9.so /proc/$gamePid/maps'" 2>$null) -join '')
                if ($mapped) { break }
            }
            Start-Sleep -Milliseconds 100
        }
        if ($gamePid -le 0 -or -not $mapped) { throw "Fresh game process did not load libAsphalt9" }
        Assert-CleanTracer $gamePid
        $start = Get-StartTime $gamePid
        $nonce = [Guid]::NewGuid().ToString('N')
        $temp = "$receipt.$nonce.tmp"
        [ordered]@{version=1;device=$Device;boot_id=(Get-BootId);pid=$gamePid;start_time=$start;nonce=$nonce;created_unix=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds();controller_sha256=$controllerHash;validator_sha256=$validatorHash} |
            ConvertTo-Json | Set-Content -LiteralPath $temp -Encoding utf8
        Move-Item -LiteralPath $temp -Destination $receipt
        Write-Output "FC3_ENTRY_PREPARED pid=$gamePid start_time=$start nonce=$nonce device_access=1 attached=0 preload=0"
        $prepared = $true
    } finally {
        if (-not $prepared) { try { Stop-FreshPackage } finally { if (Test-Path $receipt) { Remove-Item $receipt -Force } } }
    }
    return
}

foreach ($gate in @(
    @($AcknowledgeNaturalRunningRace, "natural running race"),
    @($AcknowledgeAttachReadDetachOnly, "attach/read/detach-only scope"),
    @($AcknowledgeNoHardwareWatchpointsOrDebugWrites, "zero hardware watchpoints and debug writes"),
    @($AcknowledgeNoPayloadVptrOrGameWrites, "zero payload, vptr and game writes"),
    @($AcknowledgePtraceStalls, "temporary ptrace stalls"),
    @($AcknowledgeExitKillMayTerminateFreshProcess, "EXITKILL may terminate the fresh process"),
    @($AcknowledgeFailureForceStopsFreshProcess, "force-stop on any failure")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Entry prepared-process receipt missing" }

$passed = $false
try {
    $r = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $gamePid = Get-GamePid
    if ($gamePid -le 0 -or $r.version -ne 1 -or $r.device -ne $Device -or
        $r.boot_id -ne (Get-BootId) -or [int]$r.pid -ne $gamePid -or
        [string]$r.start_time -ne (Get-StartTime $gamePid) -or
        $r.nonce -notmatch '^[0-9a-f]{32}$' -or $now -lt [long]$r.created_unix -or
        $now - [long]$r.created_unix -gt $ReceiptTtlSeconds -or
        $r.controller_sha256 -ne $controllerHash -or $r.validator_sha256 -ne $validatorHash) {
        throw "Entry receipt mismatch or expiry"
    }
    Assert-CleanTracer $gamePid
    if ((Get-RemoteHash $remoteController) -ne $controllerHash) { throw "Remote controller hash mismatch" }
    $base = Get-Base $gamePid
    $remoteUnused = "/data/local/tmp/a9tas_fc3_entry_unused_${gamePid}_$($r.nonce).bin"
    $remoteReport = "/data/local/tmp/a9tas_fc3_entry_${gamePid}_$($r.nonce).bin"
    $localReport = Join-Path $outDir "a9tas_fc3_entry_${gamePid}_$($r.nonce).bin"
    if (Test-Path -LiteralPath $localReport) { throw "Local entry report already exists" }
    $exists = ((& $AdbPath -s $Device shell "su -c 'if [ -e $remoteUnused ] || [ -e $remoteReport ]; then echo EXISTS; fi'" 2>$null) -join '').Trim()
    if ($exists) { throw "Remote entry report path already exists" }
    $lines = & $AdbPath -s $Device shell "su -c '$remoteController $gamePid $($r.start_time) $base $TimeoutMs $remoteUnused $remoteReport $ack'" 2>&1
    $code = $LASTEXITCODE
    $lines | Out-Host
    $controllerPassed = $code -eq 0 -and ($lines -join "`n") -match 'FC3_ENTRY_STABILITY_DONE success=1'
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 0444 $remoteReport'") "make entry report pull-readable" | Out-Null
    $remoteHash = Get-RemoteHash $remoteReport
    Invoke-AdbChecked @('-s', $Device, 'pull', $remoteReport, $localReport) "pull entry report" | Out-Null
    if ((Get-Sha256 $localReport) -ne $remoteHash) { throw "Entry report hash mismatch" }
    if (-not $controllerPassed) {
        Write-Output "FC3_ENTRY_FAILED_REPORT_PRESERVED report=$localReport"
        throw "Entry controller failed closed"
    }
    & $python.Source $validator $localReport
    if ($LASTEXITCODE -ne 0) { throw "Entry report validation failed" }
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTime $gamePid) -ne [string]$r.start_time) { throw "Process changed after entry probe" }
    Assert-CleanTracer $gamePid
    Start-Sleep -Milliseconds $PostProbeObservationMs
    Assert-CleanTracer $gamePid
    Remove-Item -LiteralPath $receipt -Force
    $passed = $true
    Write-Output "FC3_ENTRY_RUNNER_OK pid=$gamePid attached_then_detached=1 hwbp=0 debug_writes=0 game_writes=0"
} finally {
    if (-not $passed) { try { Stop-FreshPackage } finally { if (Test-Path $receipt) { Remove-Item $receipt -Force } } }
}
