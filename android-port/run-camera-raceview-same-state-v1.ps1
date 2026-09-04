param(
 [ValidateSet("OfflineValidateOnly","PrepareFreshProcess","ExecuteCountdownGate","Status","Rollback")]
 [string]$Mode="OfflineValidateOnly",
 [string]$Device="emulator-5554",
 [string]$AdbPath="D:\leidian\LDPlayer9\adb.exe",
 [int]$CompletionTimeoutSeconds=10,
 [switch]$AcknowledgeFreshGameProcessRestart,
 [switch]$AcknowledgePreloadWindowInjection,
 [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
 [switch]$AcknowledgeExactlyOneEscapeResume,
 [switch]$AcknowledgeFiveSameStateCallbacks,
 [switch]$AcknowledgeSingleRaceViewCallbackSlot,
 [switch]$AcknowledgeBriefWholeProcessStop,
 [switch]$AcknowledgeCrashRisk
)
$ErrorActionPreference="Stop"
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$package="com.aligames.kuang.kybc.aligames"
$activity="$package/$package.MainActivity"
$outDir=Join-Path $root "build\camera-raceview-same-state-runner-v1"
$payload=Join-Path $root "build\camera-raceview-same-state-v1\liba9tas_camera_raceview_same_state_v1_build_only.so"
$transactionDir=Join-Path $root "build\camera-raceview-same-state-transaction-v1"
$controller=Join-Path $transactionDir "a9tas_camera_raceview_same_state_transaction_v1"
$bootstrap=Join-Path $transactionDir "liba9tas_bootstrap_camera_raceview_same_state_v1.so"
$managerGraph=Join-Path $root "build\camera-manager-graph-v1\a9tas_camera_manager_graph_check_v1_review_only"
$injector=Join-Path $root "build\a9tas_injector"
$helper=Join-Path $root "tools\run_camera_raceview_same_state_preload_v1.sh"
$policy=Join-Path $root "tools\test_run_camera_raceview_same_state_policy_v1.py"
$validator=Join-Path $root "tools\validate_camera_raceview_same_state_report_v1.py"
$preparedReceipt=Join-Path $outDir "prepared-process-v1.json"
$activeReceipt=Join-Path $outDir "active-gate-v1.json"
$installReceipt=Join-Path $outDir "same-state-install-v1.bin"
$finalReceipt=Join-Path $outDir "same-state-final-v1.bin"
$rollbackReceipt=Join-Path $outDir "same-state-rollback-v1.bin"
$pins=@{
 $payload="58e888e4fb606fe3ec1b497c97a2a64c0b4dfc7dbd647ad0c01fa0558efd6ddf"
 $controller="fb040864d2107f075864152b9618a29dcbd8bcaae80f8c3a776ffc2831df88a8"
 $bootstrap="dbad61a2ebc3b25e44c586442dc7285cc3a976fbdf332dd043585d46d21748fc"
 $managerGraph="977ea2734fcc8b0e2256f8d533f539d310be9001124f4fd9dc87795cee4f1ef0"
 $injector="b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
 $helper="65786617a93eb3386da8af2b0a863cf31210ffbed3a61fcafd33e5daab15964e"
 $validator="f8804e3a31fee7520426b0851ab919d5131c9928c96d1ee0cc58dc50ca764fee"
}
$remotePayload="/data/local/tmp/liba9tas_camera_raceview_same_state_v1_build_only.so"
$remoteController="/data/local/tmp/a9tas_camera_raceview_same_state_transaction_v1"
$remoteBootstrap="/data/local/tmp/liba9tas_bootstrap_camera_raceview_same_state_v1.so"
$remoteManagerGraph="/data/local/tmp/a9tas_camera_manager_graph_v1"
$remoteInjector="/data/local/tmp/a9tas_injector_camera_same_state_v1"
$remoteHelper="/data/local/tmp/run_camera_raceview_same_state_preload_v1.sh"
$remoteInstall="/data/local/tmp/a9tas_camera_same_state_install_v1.bin"
$remoteStatus="/data/local/tmp/a9tas_camera_same_state_status_v1.bin"
$remoteFinal="/data/local/tmp/a9tas_camera_same_state_final_v1.bin"
$remoteRollback="/data/local/tmp/a9tas_camera_same_state_rollback_v1.bin"
$ack="I_ACCEPT_RACEVIEW_SAME_STATE_SINGLE_SLOT_V1"
$managerAck="I_ACCEPT_CAMERA_MANAGER_GRAPH_READ_ONLY_V1"
New-Item $outDir -ItemType Directory -Force|Out-Null

function Get-Sha256([string]$Path) {
    (Get-FileHash $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path $artifact -PathType Leaf) -or
            (Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Pinned same-state artifact mismatch: $artifact"
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
function Get-StartTicks([int]$GameProcessId) {
    $text = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat /proc/$GameProcessId/stat'")
    if ($text -notmatch '^\d+\s+\(.*\)\s+\S\s+(.+)$') { throw "stat parse failed" }
    $fields = @($matches[1] -split '\s+')
    if ($fields.Count -lt 19 -or $fields[18] -notmatch '^\d+$') { throw "start ticks parse failed" }
    $fields[18]
}
function Get-GameBase([int]$GameProcessId) {
    $maps = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat /proc/$GameProcessId/maps'")
    $raw = @(foreach ($line in ($maps -split "`n")) {
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+0+\s+.*libAsphalt9\.so') {
            [Convert]::ToUInt64($matches[1],16)
        }
    })
    $values = @($raw | Sort-Object -Unique)
    if ($values.Count -ne 1) { throw "game base resolution failed" }
    $values[0].ToString('x')
}
function Assert-CleanTracer([int]$GameProcessId) {
    $line = Invoke-AdbText @('-s',$Device,'shell',"su -c 'grep ^TracerPid: /proc/$GameProcessId/status'")
    if ($line -notin @("TracerPid:`t0","TracerPid: 0")) { throw "Active tracer: $line" }
}
function Assert-RemoteHash([string]$Path,[string]$Expected) {
    $line = Invoke-AdbText @('-s',$Device,'shell',"su -c 'sha256sum $Path'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) { throw "Remote hash mismatch: $Path" }
}
function Assert-Prepared {
    if (-not (Test-Path $preparedReceipt -PathType Leaf)) { throw "Prepared receipt missing" }
    $prepared = Get-Content -Raw $preparedReceipt | ConvertFrom-Json
    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0 -or [int]$prepared.pid -ne $gameProcessId -or
        [string]$prepared.start_ticks -ne (Get-StartTicks $gameProcessId) -or
        $prepared.payload_sha256 -ne $pins[$payload] -or
        $prepared.controller_sha256 -ne $pins[$controller] -or
        $prepared.bootstrap_sha256 -ne $pins[$bootstrap]) { throw "Prepared process mismatch" }
    Assert-CleanTracer $gameProcessId
    foreach ($pair in @(
        @($remotePayload,$pins[$payload]), @($remoteController,$pins[$controller]),
        @($remoteBootstrap,$pins[$bootstrap]), @($remoteManagerGraph,$pins[$managerGraph])
    )) { Assert-RemoteHash $pair[0] $pair[1] }
    $maps = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat /proc/$gameProcessId/maps'")
    if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
        throw "Prepared mappings missing"
    }
    $prepared
}
function Resolve-RaceView([int]$GameProcessId,[string]$StartTicks,[string]$GameBase) {
    $text = Invoke-AdbText @('-s',$Device,'shell',"su -c '$remoteManagerGraph $GameProcessId $GameBase $StartTicks $managerAck'")
    $head = [regex]::Match($text,'(?m)^CAMERA_MANAGER_GRAPH_V1 .*resolve_result=0 graph_result=0 .*manager=0x(?<manager>[0-9a-fA-F]+) ')
    $shape = [regex]::Match($text,'(?m)^CAMERA_MANAGER_FIELD offset=0xe8 value=0x(?<shape>[0-9a-fA-F]+) ')
    if (-not $head.Success -or -not $shape.Success) { throw "RaceView identity resolution failed" }
    [ordered]@{manager=$head.Groups['manager'].Value.ToLowerInvariant();shape=$shape.Groups['shape'].Value.ToLowerInvariant()}
}
function Invoke-Transaction([string]$Action,[object]$Session,[string]$RemoteOutput) {
    Invoke-AdbText @('-s',$Device,'shell',"su -c 'rm -f $RemoteOutput; $remoteController $Action $($Session.pid) $($Session.start_ticks) $($Session.game_base) $($Session.manager) $($Session.shape) $RemoteOutput $ack'")
}
function Invoke-Rollback([object]$Session) {
    try {
        $text = Invoke-Transaction 'rollback' $Session $remoteRollback
        $text | Write-Host
        Remove-Item $rollbackReceipt -Force -ErrorAction SilentlyContinue
        Invoke-AdbText @('-s',$Device,'pull',$remoteRollback,$rollbackReceipt) | Out-Null
    } finally {
        Remove-Item $activeReceipt -Force -ErrorAction SilentlyContinue
    }
}

Assert-LocalArtifacts
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "same-state runner policy failed" }
if ($CompletionTimeoutSeconds -lt 5 -or $CompletionTimeoutSeconds -gt 30) {
    throw "Completion timeout must be 5..30"
}
if ($Mode -eq "OfflineValidateOnly") {
    Write-Output "CAMERA_RACEVIEW_SAME_STATE_RUNNER_OFFLINE_OK device_access=0 deployed=0 game_writes=0"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB unavailable" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "LDPlayer unavailable"
}

if ($Mode -eq "PrepareFreshProcess") {
    if (-not $AcknowledgeFreshGameProcessRestart -or -not $AcknowledgePreloadWindowInjection) {
        throw "Restart/preload acknowledgements required"
    }
    Invoke-AdbText @('-s',$Device,'shell',"am force-stop $package") | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old process remained alive" }
    foreach ($pair in @(
        @($payload,$remotePayload), @($controller,$remoteController),
        @($bootstrap,$remoteBootstrap), @($managerGraph,$remoteManagerGraph),
        @($injector,$remoteInjector), @($helper,$remoteHelper)
    )) { Invoke-AdbText @('-s',$Device,'push',$pair[0],$pair[1]) | Out-Null }
    Invoke-AdbText @('-s',$Device,'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteManagerGraph $remoteInjector $remoteHelper'") | Out-Null
    foreach ($pair in @(
        @($remotePayload,$pins[$payload]), @($remoteController,$pins[$controller]),
        @($remoteBootstrap,$pins[$bootstrap]), @($remoteManagerGraph,$pins[$managerGraph]),
        @($remoteInjector,$pins[$injector]), @($remoteHelper,$pins[$helper])
    )) { Assert-RemoteHash $pair[0] $pair[1] }
    $stdout = Join-Path $outDir 'preload.stdout.txt'
    $stderr = Join-Path $outDir 'preload.stderr.txt'
    Remove-Item $stdout,$stderr -Force -ErrorAction SilentlyContinue
    $preload = Start-Process $AdbPath -ArgumentList @('-s',$Device,'shell','sh',$remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 300
    Invoke-AdbText @('-s',$Device,'shell',"am start -n $activity") | Out-Null
    if (-not $preload.WaitForExit(45000)) {
        Stop-Process -Id $preload.Id -Force
        throw "preload timeout"
    }
    $preload.Refresh()
    $text = (Get-Content -Raw $stdout) + "`n" + (Get-Content -Raw $stderr)
    $text | Write-Host
    $proof = [regex]::Match($text,'(?m)^pid=(?<pid>\d+) tid=\d+ bootstrap_verified=1 status=1 stage=2\s*$')
    if (-not $proof.Success) { throw "preload proof missing" }
    $gameProcessId = Get-GamePid
    if ($gameProcessId -ne [int]$proof.Groups['pid'].Value) { throw "preload PID mismatch" }
    $mapped = $false
    for ($i=0; $i -lt 300; ++$i) {
        $maps = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat /proc/$gameProcessId/maps'")
        if ($maps.Contains($remotePayload) -and $maps.Contains($remoteBootstrap)) {$mapped=$true;break}
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped) { throw "payload mapping missing" }
    Assert-CleanTracer $gameProcessId
    [ordered]@{version=1;device=$Device;pid=$gameProcessId;start_ticks=(Get-StartTicks $gameProcessId);payload_sha256=$pins[$payload];controller_sha256=$pins[$controller];bootstrap_sha256=$pins[$bootstrap]} |
        ConvertTo-Json | Set-Content $preparedReceipt -Encoding utf8
    Remove-Item $activeReceipt -Force -ErrorAction SilentlyContinue
    Write-Output "CAMERA_RACEVIEW_SAME_STATE_PREPARED pid=$gameProcessId game_writes=0 callback_installed=0"
    return
}

$prepared = Assert-Prepared
if ($Mode -eq "Status" -or $Mode -eq "Rollback") {
    if (-not (Test-Path $activeReceipt -PathType Leaf)) { throw "Active gate receipt missing" }
    $session = Get-Content -Raw $activeReceipt | ConvertFrom-Json
    if ($Mode -eq "Rollback") {
        Invoke-Rollback $session
        Write-Output "CAMERA_RACEVIEW_SAME_STATE_ROLLBACK_DONE"
        return
    }
    Invoke-Transaction 'status' $session $remoteStatus | Write-Output
    return
}
foreach ($gate in @(
    @($AcknowledgeAncientRuinsZl1Countdown3Paused,'countdown-3 pause'),
    @($AcknowledgeExactlyOneEscapeResume,'one ESC'),
    @($AcknowledgeFiveSameStateCallbacks,'five same-state callbacks'),
    @($AcknowledgeSingleRaceViewCallbackSlot,'single callback slot'),
    @($AcknowledgeBriefWholeProcessStop,'brief process stops'),
    @($AcknowledgeCrashRisk,'remaining crash risk')
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }

$gameProcessId = [int]$prepared.pid
$startTicks = [string]$prepared.start_ticks
$gameBase = Get-GameBase $gameProcessId
$identity = Resolve-RaceView $gameProcessId $startTicks $gameBase
$session = [ordered]@{version=1;pid=$gameProcessId;start_ticks=$startTicks;game_base=$gameBase;manager=$identity.manager;shape=$identity.shape}
$session | ConvertTo-Json | Set-Content $activeReceipt -Encoding utf8
$completed = $false
try {
    Remove-Item $installReceipt,$finalReceipt -Force -ErrorAction SilentlyContinue
    Invoke-Transaction 'install' $session $remoteInstall | Write-Host
    Invoke-AdbText @('-s',$Device,'pull',$remoteInstall,$installReceipt) | Out-Null
    Invoke-AdbText @('-s',$Device,'shell','input keyevent 111') | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds($CompletionTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $statusText = Invoke-Transaction 'status' $session $remoteStatus
        if ($statusText -match 'RACEVIEW_SAME_STATE_STATUS complete=1') {$completed=$true;break}
    }
    if (-not $completed) { throw "five-callback gate timeout" }
    Invoke-Transaction 'finalize' $session $remoteFinal | Write-Host
    Invoke-AdbText @('-s',$Device,'pull',$remoteFinal,$finalReceipt) | Out-Null
    python -B $validator $finalReceipt
    if ($LASTEXITCODE -ne 0) { throw "final same-state receipt validation failed" }
    Assert-CleanTracer $gameProcessId
    Remove-Item $activeReceipt -Force
    Write-Output "CAMERA_RACEVIEW_SAME_STATE_GATE_PASSED callbacks=5 manager=0x$($identity.manager) shape=0x$($identity.shape) report=$finalReceipt"
} catch {
    if ((Get-GamePid) -gt 0) {
        try { Invoke-Rollback $session }
        catch { Write-Warning "Automatic rollback failed: $_" }
    }
    throw
}
