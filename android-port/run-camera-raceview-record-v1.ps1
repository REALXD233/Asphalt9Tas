param(
    [ValidateSet("OfflineValidateOnly", "PrepareFreshProcess", "ExecuteCountdownCapture", "Status", "Rollback")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$Frames = 360,
    [int]$CompletionTimeoutSeconds = 20,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeExactlyOneEscapeResume,
    [switch]$AcknowledgeSingleRaceViewCallbackSlot,
    [switch]$AcknowledgeBriefWholeProcessStop,
    [switch]$AcknowledgeCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\camera-raceview-record-runner-v1"
$payload = Join-Path $root "build\camera-raceview-record-v1\liba9tas_camera_raceview_record_v1_build_only.so"
$transactionDir = Join-Path $root "build\camera-raceview-record-transaction-v1"
$controller = Join-Path $transactionDir "a9tas_camera_raceview_record_transaction_v1"
$bootstrap = Join-Path $transactionDir "liba9tas_bootstrap_camera_raceview_record_v1.so"
$managerGraph = Join-Path $root "build\camera-manager-graph-v1\a9tas_camera_manager_graph_check_v1_review_only"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_camera_raceview_record_preload_v1.sh"
$runnerPolicy = Join-Path $root "tools\test_run_camera_raceview_record_policy_v1.py"
$preparedReceipt = Join-Path $outDir "prepared-process-v1.json"
$activeReceipt = Join-Path $outDir "active-capture-v1.json"
$recording = Join-Path $outDir "camera-raceview-recording-v1.bin"
$installReceipt = Join-Path $outDir "camera-raceview-install-v1.bin"
$rollbackReceipt = Join-Path $outDir "camera-raceview-rollback-v1.bin"

$pins = @{
    $payload = "c253eecb2244756edb53d54ad5f546301522378b7c83e4bbaaa1d491342b2639"
    $controller = "5e94b69c335dcdb22e0efb46888cadceb3903d216f5d189f458aa99313678a0a"
    $bootstrap = "88a31fbd698fc18652a43479b03437e48a76a7361767f1431645b2153abf4077"
    $managerGraph = "977ea2734fcc8b0e2256f8d533f539d310be9001124f4fd9dc87795cee4f1ef0"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "37f3939a6fe92406ad3ec2b04f2a84c137b32dcd37a87f3b3dfdf2c4140dedef"
}

$remotePayload = "/data/local/tmp/liba9tas_camera_raceview_record_v1_build_only.so"
$remoteController = "/data/local/tmp/a9tas_camera_raceview_record_transaction_v1"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_camera_raceview_record_v1.so"
$remoteManagerGraph = "/data/local/tmp/a9tas_camera_manager_graph_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_camera_raceview_v1"
$remoteHelper = "/data/local/tmp/run_camera_raceview_record_preload_v1.sh"
$remoteInstall = "/data/local/tmp/a9tas_camera_raceview_install_v1.bin"
$remoteStatus = "/data/local/tmp/a9tas_camera_raceview_status_v1.bin"
$remoteRecording = "/data/local/tmp/a9tas_camera_raceview_recording_v1.bin"
$remoteRollback = "/data/local/tmp/a9tas_camera_raceview_rollback_v1.bin"
$transactionAck = "I_ACCEPT_RACEVIEW_RECORD_SINGLE_SLOT_V1"
$managerAck = "I_ACCEPT_CAMERA_MANAGER_GRAPH_READ_ONLY_V1"

New-Item -ItemType Directory -Path $outDir -Force | Out-Null

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
            (Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Pinned RaceView artifact mismatch: $artifact"
        }
    }
}
function Invoke-AdbText([string[]]$Arguments) {
    $text = ((& $AdbPath @Arguments 2>&1) -join "`n").Trim()
    if ($LASTEXITCODE -ne 0) { throw "ADB command failed: $text" }
    $text
}
function Get-GamePid {
    $text = ((& $AdbPath -s $Device shell "su -c 'pidof $package'" 2>$null) -join '').Trim()
    if ($text -match '^\d+$') { return [int]$text }
    0
}
function Get-StartTicks([int]$GamePid) {
    $text = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    if ($text -notmatch '^\d+\s+\(.*\)\s+\S\s+(.+)$') { throw "Process stat parse failed" }
    $fields = @($matches[1] -split '\s+')
    if ($fields.Count -lt 19 -or $fields[18] -notmatch '^\d+$') { throw "Process start ticks parse failed" }
    $fields[18]
}
function Get-GameBase([int]$GamePid) {
    $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/maps'")
    $rawValues = @(foreach ($line in ($maps -split "`n")) {
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+0+\s+.*libAsphalt9\.so') {
            [Convert]::ToUInt64($matches[1], 16)
        }
    })
    $values = @($rawValues | Sort-Object -Unique)
    if ($values.Count -ne 1) { throw "libAsphalt9 base resolution failed" }
    $values[0].ToString('x')
}
function Assert-CleanTracer([int]$GamePid) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($line -notin @("TracerPid:`t0", "TracerPid: 0")) { throw "Active tracer: $line" }
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Path'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) { throw "Remote hash mismatch: $Path" }
}
function Assert-Prepared {
    if (-not (Test-Path -LiteralPath $preparedReceipt -PathType Leaf)) { throw "Prepared-process receipt missing" }
    $prepared = Get-Content -Raw $preparedReceipt | ConvertFrom-Json
    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0 -or [int]$prepared.pid -ne $gameProcessId -or
        [string]$prepared.start_ticks -ne (Get-StartTicks $gameProcessId) -or
        $prepared.payload_sha256 -ne $pins[$payload] -or
        $prepared.controller_sha256 -ne $pins[$controller] -or
        $prepared.bootstrap_sha256 -ne $pins[$bootstrap]) { throw "Prepared process mismatch" }
    Assert-CleanTracer $gameProcessId
    foreach ($pair in @(@($remotePayload,$pins[$payload]),@($remoteController,$pins[$controller]),@($remoteBootstrap,$pins[$bootstrap]),@($remoteManagerGraph,$pins[$managerGraph]))) {
        Assert-RemoteHash $pair[0] $pair[1]
    }
    $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gameProcessId/maps'")
    if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) { throw "Prepared payload mappings missing" }
    $prepared
}
function Resolve-RaceView([int]$GamePid, [string]$StartTicks, [string]$GameBase) {
    $text = Invoke-AdbText @('-s', $Device, 'shell', "su -c '$remoteManagerGraph $GamePid $GameBase $StartTicks $managerAck'")
    $head = [regex]::Match($text, '(?m)^CAMERA_MANAGER_GRAPH_V1 .*resolve_result=0 graph_result=0 .*manager=0x(?<manager>[0-9a-fA-F]+) ')
    $shape = [regex]::Match($text, '(?m)^CAMERA_MANAGER_FIELD offset=0xe8 value=0x(?<shape>[0-9a-fA-F]+) ')
    if (-not $head.Success -or -not $shape.Success) { throw "RaceView manager/shape identity resolution failed" }
    [ordered]@{ manager=$head.Groups['manager'].Value.ToLowerInvariant(); shape=$shape.Groups['shape'].Value.ToLowerInvariant() }
}
function Invoke-Transaction([string]$Action, [object]$Session, [string]$RemoteOutput) {
    Invoke-AdbText @('-s', $Device, 'shell', "su -c 'rm -f $RemoteOutput; $remoteController $Action $($Session.pid) $($Session.start_ticks) $($Session.game_base) $($Session.manager) $($Session.shape) $($Session.frames) $RemoteOutput $transactionAck'")
}
function Invoke-Rollback([object]$Session) {
    try {
        $text = Invoke-Transaction 'rollback' $Session $remoteRollback
        $text | Write-Host
        Remove-Item $rollbackReceipt -Force -ErrorAction SilentlyContinue
        Invoke-AdbText @('-s', $Device, 'pull', $remoteRollback, $rollbackReceipt) | Out-Null
    } finally {
        Remove-Item $activeReceipt -Force -ErrorAction SilentlyContinue
    }
}

Assert-LocalArtifacts
python -B $runnerPolicy
if ($LASTEXITCODE -ne 0) { throw "RaceView runner policy failed" }
if ($Frames -lt 30 -or $Frames -gt 3600) { throw "Frames must be 30..3600" }
if ($CompletionTimeoutSeconds -lt 5 -or $CompletionTimeoutSeconds -gt 60) { throw "CompletionTimeoutSeconds must be 5..60" }

if ($Mode -eq "OfflineValidateOnly") {
    Write-Output "CAMERA_RACEVIEW_RECORD_RUNNER_OFFLINE_OK device_access=0 deployed=0 game_writes=0"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB unavailable" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") { throw "LDPlayer device unavailable" }

if ($Mode -eq "PrepareFreshProcess") {
    if (-not $AcknowledgeFreshGameProcessRestart -or -not $AcknowledgePreloadWindowInjection) {
        throw "Fresh-process restart and preload acknowledgements are required"
    }
    Invoke-AdbText @('-s', $Device, 'shell', "am force-stop $package") | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    foreach ($pair in @(@($payload,$remotePayload),@($controller,$remoteController),@($bootstrap,$remoteBootstrap),@($managerGraph,$remoteManagerGraph),@($injector,$remoteInjector),@($helper,$remoteHelper))) {
        Invoke-AdbText @('-s', $Device, 'push', $pair[0], $pair[1]) | Out-Null
    }
    Invoke-AdbText @('-s', $Device, 'shell', "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteManagerGraph $remoteInjector $remoteHelper'") | Out-Null
    foreach ($pair in @(@($remotePayload,$pins[$payload]),@($remoteController,$pins[$controller]),@($remoteBootstrap,$pins[$bootstrap]),@($remoteManagerGraph,$pins[$managerGraph]),@($remoteInjector,$pins[$injector]),@($remoteHelper,$pins[$helper]))) { Assert-RemoteHash $pair[0] $pair[1] }
    $stdout = Join-Path $outDir 'preload.stdout.txt'
    $stderr = Join-Path $outDir 'preload.stderr.txt'
    Remove-Item $stdout,$stderr -Force -ErrorAction SilentlyContinue
    $preload = Start-Process -FilePath $AdbPath -ArgumentList @('-s',$Device,'shell','sh',$remoteHelper) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 300
    Invoke-AdbText @('-s', $Device, 'shell', "am start -n $activity") | Out-Null
    if (-not $preload.WaitForExit(45000)) { Stop-Process $preload.Id -Force; throw "RaceView preload timed out" }
    $preload.Refresh()
    $text = (Get-Content -Raw $stdout) + "`n" + (Get-Content -Raw $stderr)
    $text | Write-Host
    $proof = [regex]::Match($text, '(?m)^pid=(?<pid>\d+) tid=\d+ bootstrap_verified=1 status=1 stage=2\s*$')
    if (-not $proof.Success) { throw "RaceView preload proof missing" }
    $gameProcessId = Get-GamePid
    if ($gameProcessId -ne [int]$proof.Groups['pid'].Value) { throw "Preloaded process identity mismatch" }
    $mapped = $false
    for ($i=0; $i -lt 300; ++$i) {
        $maps = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat /proc/$gameProcessId/maps'")
        if ($maps.Contains($remotePayload) -and $maps.Contains($remoteBootstrap)) { $mapped=$true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped) { throw "RaceView payload mapping missing" }
    Assert-CleanTracer $gameProcessId
    [ordered]@{version=1;device=$Device;pid=$gameProcessId;start_ticks=(Get-StartTicks $gameProcessId);payload_sha256=$pins[$payload];controller_sha256=$pins[$controller];bootstrap_sha256=$pins[$bootstrap]} | ConvertTo-Json | Set-Content $preparedReceipt -Encoding utf8
    Remove-Item $activeReceipt -Force -ErrorAction SilentlyContinue
    Write-Output "CAMERA_RACEVIEW_PREPARED pid=$gameProcessId game_writes=0 callback_installed=0"
    return
}

$prepared = Assert-Prepared
if ($Mode -eq "Status" -or $Mode -eq "Rollback") {
    if (-not (Test-Path $activeReceipt -PathType Leaf)) { throw "Active capture receipt missing" }
    $session = Get-Content -Raw $activeReceipt | ConvertFrom-Json
    if ($Mode -eq "Rollback") { Invoke-Rollback $session; Write-Output "CAMERA_RACEVIEW_ROLLBACK_DONE"; return }
    $text = Invoke-Transaction 'status' $session $remoteStatus
    $text | Write-Output
    return
}

foreach ($gate in @(
    @($AcknowledgeAncientRuinsZl1Countdown3Paused,'Ancient Ruins plus ZL1 countdown-3 pause'),
    @($AcknowledgeExactlyOneEscapeResume,'exactly one ESC resume'),
    @($AcknowledgeSingleRaceViewCallbackSlot,'the single RaceView callback-slot transaction'),
    @($AcknowledgeBriefWholeProcessStop,'two brief whole-process stops'),
    @($AcknowledgeCrashRisk,'the remaining crash risk')
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }

$gameProcessId = [int]$prepared.pid
$startTicks = [string]$prepared.start_ticks
$gameBase = Get-GameBase $gameProcessId
$identity = Resolve-RaceView $gameProcessId $startTicks $gameBase
$session = [ordered]@{version=1;pid=$gameProcessId;start_ticks=$startTicks;game_base=$gameBase;manager=$identity.manager;shape=$identity.shape;frames=$Frames}
$session | ConvertTo-Json | Set-Content $activeReceipt -Encoding utf8
$completed = $false
try {
    Remove-Item $installReceipt,$recording -Force -ErrorAction SilentlyContinue
    $installText = Invoke-Transaction 'install' $session $remoteInstall
    $installText | Write-Host
    Invoke-AdbText @('-s',$Device,'pull',$remoteInstall,$installReceipt) | Out-Null
    Invoke-AdbText @('-s',$Device,'shell','input keyevent 111') | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds($CompletionTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $statusText = Invoke-Transaction 'status' $session $remoteStatus
        if ($statusText -match 'RACEVIEW_RECORD_STATUS complete=1') { $completed=$true; break }
    }
    if (-not $completed) { throw "RaceView capture did not complete before timeout" }
    $finalText = Invoke-Transaction 'finalize' $session $remoteRecording
    $finalText | Write-Host
    Invoke-AdbText @('-s',$Device,'pull',$remoteRecording,$recording) | Out-Null
    Assert-CleanTracer $gameProcessId
    Remove-Item $activeReceipt -Force
    Write-Output "CAMERA_RACEVIEW_CAPTURE_PASSED frames=$Frames manager=0x$($identity.manager) shape=0x$($identity.shape) recording=$recording"
} catch {
    if ((Get-GamePid) -gt 0) { try { Invoke-Rollback $session } catch { Write-Warning "Automatic rollback could not complete: $_" } }
    throw
}
