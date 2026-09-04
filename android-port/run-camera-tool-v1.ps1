param(
    [ValidateSet("OfflineValidateOnly","PrepareFreshProcess","Install","SetAbsolute","Disable","Status","Uninstall")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$OverrideFlags = 7,
    [double]$PositionX = 0,
    [double]$PositionY = 0,
    [double]$PositionZ = 0,
    [double]$RotationX = 0,
    [double]$RotationY = 0,
    [double]$RotationZ = 0,
    [double]$RotationW = 1,
    [double]$FovRadians = 1,
    [switch]$AcknowledgeFreshGameProcessRestart,
    [switch]$AcknowledgePreloadWindowInjection,
    [switch]$AcknowledgeRacePaused,
    [switch]$AcknowledgeSingleRaceViewCallbackSlot,
    [switch]$AcknowledgeBriefWholeProcessStop,
    [switch]$AcknowledgeCameraOverrideWrites,
    [switch]$AcknowledgeCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$outDir = Join-Path $root "build\camera-tool-runner-v1"
$payload = Join-Path $root "build\camera-tool-v1\liba9tas_camera_tool_v1_build_only.so"
$transactionDir = Join-Path $root "build\camera-tool-transaction-v1"
$controller = Join-Path $transactionDir "a9tas_camera_tool_transaction_v1"
$bootstrap = Join-Path $transactionDir "liba9tas_bootstrap_camera_tool_v1.so"
$managerGraph = Join-Path $root "build\camera-manager-graph-v1\a9tas_camera_manager_graph_check_v1_review_only"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_camera_tool_preload_v1.sh"
$inspector = Join-Path $root "tools\inspect_camera_tool_report_v1.py"
$buildScript = Join-Path $root "build-camera-tool-transaction-v1.ps1"
$preparedReceipt = Join-Path $outDir "prepared-process-v1.json"
$activeReceipt = Join-Path $outDir "active-camera-tool-v1.json"
$localReport = Join-Path $outDir "camera-tool-last-v1.bin"
$pins = @{
    $payload = "7b2fad97e599943ff6c41d67611caeeb82d517fae5545c1e3f228371b3bdf5c8"
    $controller = "cc622485127f2256ca79a5199e82e916a18bd3d6a5ec79cd2b58f8fe19968f94"
    $bootstrap = "38565f046db837e3c3d5b39142a9eed0d6d90eb78b2e66fc02cb197affe89fd8"
    $managerGraph = "977ea2734fcc8b0e2256f8d533f539d310be9001124f4fd9dc87795cee4f1ef0"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "0e427c9c7593434408b6d1d543e0d8508443fb92099d762fd8ff09f00cc5416c"
    $inspector = "8a7b0cce7d24fca455fddaf8715746eb145acb0a5c4d3f0dfbfbccdf7b49edbd"
}
$remotePayload = "/data/local/tmp/liba9tas_camera_tool_v1_build_only.so"
$remoteController = "/data/local/tmp/a9tas_camera_tool_transaction_v1"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_camera_tool_v1.so"
$remoteManagerGraph = "/data/local/tmp/a9tas_camera_manager_graph_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_camera_tool_v1"
$remoteHelper = "/data/local/tmp/run_camera_tool_preload_v1.sh"
$remoteReport = "/data/local/tmp/a9tas_camera_tool_report_v1.bin"
$ack = "I_ACCEPT_CAMERA_TOOL_SINGLE_SLOT_V1"
$managerAck = "I_ACCEPT_CAMERA_MANAGER_GRAPH_READ_ONLY_V1"
$invariant = [Globalization.CultureInfo]::InvariantCulture

New-Item -Path $outDir -ItemType Directory -Force | Out-Null

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
            (Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Pinned Camera Tool artifact mismatch: $artifact"
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
    if ($line -notin @("TracerPid:`t0","TracerPid: 0")) { throw "active tracer: $line" }
}

function Assert-RemoteHash([string]$Path,[string]$Expected) {
    $line = Invoke-AdbText @('-s',$Device,'shell',"su -c 'sha256sum $Path'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) { throw "remote hash mismatch: $Path" }
}

function Resolve-RaceView([int]$GameProcessId,[string]$StartTicks,[string]$GameBase) {
    $text = Invoke-AdbText @('-s',$Device,'shell',"su -c '$remoteManagerGraph $GameProcessId $GameBase $StartTicks $managerAck'")
    $head = [regex]::Match($text,'(?m)^CAMERA_MANAGER_GRAPH_V1 .*resolve_result=0 graph_result=0 .*manager=0x(?<manager>[0-9a-fA-F]+) ')
    $shapeMatch = [regex]::Match($text,'(?m)^CAMERA_MANAGER_FIELD offset=0xe8 value=0x(?<shape>[0-9a-fA-F]+) ')
    if (-not $head.Success -or -not $shapeMatch.Success) { throw "RaceView resolution failed" }
    [ordered]@{
        manager = $head.Groups['manager'].Value.ToLowerInvariant()
        shape = $shapeMatch.Groups['shape'].Value.ToLowerInvariant()
    }
}

function Assert-Prepared {
    if (-not (Test-Path -LiteralPath $preparedReceipt -PathType Leaf)) { throw "prepared receipt missing" }
    $prepared = Get-Content -Raw -LiteralPath $preparedReceipt | ConvertFrom-Json
    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0 -or [int]$prepared.pid -ne $gameProcessId -or
        [string]$prepared.start_ticks -ne (Get-StartTicks $gameProcessId)) {
        throw "prepared process mismatch"
    }
    Assert-CleanTracer $gameProcessId
    $prepared
}

function Assert-Session {
    $prepared = Assert-Prepared
    if (-not (Test-Path -LiteralPath $activeReceipt -PathType Leaf)) { throw "active Camera Tool receipt missing" }
    $session = Get-Content -Raw -LiteralPath $activeReceipt | ConvertFrom-Json
    if ([int]$session.pid -ne [int]$prepared.pid -or
        [string]$session.start_ticks -ne [string]$prepared.start_ticks) {
        throw "active Camera Tool identity mismatch"
    }
    $session
}

function Invoke-Transaction([string]$Action,[object]$Session,[string[]]$Extra = @()) {
    Invoke-AdbText @('-s',$Device,'shell',"su -c 'rm -f $remoteReport'") | Out-Null
    $base = @($Action,$Session.pid,$Session.start_ticks,$Session.game_base,$Session.manager,$Session.shape,$remoteReport)
    $quoted = (($base + $Extra + @($ack)) -join ' ')
    $text = Invoke-AdbText @('-s',$Device,'shell',"su -c '$remoteController $quoted'")
    Remove-Item -LiteralPath $localReport -Force -ErrorAction SilentlyContinue
    Invoke-AdbText @('-s',$Device,'pull',$remoteReport,$localReport) | Out-Null
    python -B $inspector $localReport
    if ($LASTEXITCODE -ne 0) { throw "Camera Tool report validation failed" }
    $text
}

& $buildScript | Write-Host
Assert-LocalArtifacts
python -B (Join-Path $root 'tools\test_camera_tool_semantics_v1.py')
if ($LASTEXITCODE -ne 0) { throw "Camera Tool source semantics failed" }
if ($Mode -eq 'OfflineValidateOnly') {
    Write-Output "CAMERA_TOOL_RUNNER_OFFLINE_OK device_access=0 deployed=0 game_writes=0"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB unavailable" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "LDPlayer unavailable"
}

if ($Mode -eq 'PrepareFreshProcess') {
    if (-not $AcknowledgeFreshGameProcessRestart -or -not $AcknowledgePreloadWindowInjection) {
        throw "restart and preload acknowledgements required"
    }
    Invoke-AdbText @('-s',$Device,'shell',"am force-stop $package") | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "old process remained alive" }
    foreach ($pair in @(
        @($payload,$remotePayload),@($controller,$remoteController),
        @($bootstrap,$remoteBootstrap),@($managerGraph,$remoteManagerGraph),
        @($injector,$remoteInjector),@($helper,$remoteHelper)
    )) { Invoke-AdbText @('-s',$Device,'push',$pair[0],$pair[1]) | Out-Null }
    Invoke-AdbText @('-s',$Device,'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteManagerGraph $remoteInjector $remoteHelper'") | Out-Null
    foreach ($pair in @(
        @($remotePayload,$pins[$payload]),@($remoteController,$pins[$controller]),
        @($remoteBootstrap,$pins[$bootstrap]),@($remoteManagerGraph,$pins[$managerGraph]),
        @($remoteInjector,$pins[$injector]),@($remoteHelper,$pins[$helper])
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
    $proof = [regex]::Match($text,'(?m)^pid=(?<pid>\d+) tid=\d+ bootstrap_verified=1 status=1 stage=2\s*$')
    if (-not $proof.Success) { throw "preload proof missing: $text" }
    $gameProcessId = Get-GamePid
    if ($gameProcessId -ne [int]$proof.Groups['pid'].Value) { throw "preload PID mismatch" }
    Assert-CleanTracer $gameProcessId
    [ordered]@{version=1;device=$Device;pid=$gameProcessId;start_ticks=(Get-StartTicks $gameProcessId)} |
        ConvertTo-Json | Set-Content -LiteralPath $preparedReceipt -Encoding utf8
    Remove-Item -LiteralPath $activeReceipt -Force -ErrorAction SilentlyContinue
    Write-Output "CAMERA_TOOL_PREPARED pid=$gameProcessId callback_installed=0 game_writes=0"
    return
}

if ($Mode -eq 'Install') {
    if (-not $AcknowledgeRacePaused -or -not $AcknowledgeSingleRaceViewCallbackSlot -or
        -not $AcknowledgeBriefWholeProcessStop -or -not $AcknowledgeCrashRisk) {
        throw "paused-race, single-slot, stop and crash-risk acknowledgements required"
    }
    $prepared = Assert-Prepared
    $gameBase = Get-GameBase ([int]$prepared.pid)
    $identity = Resolve-RaceView ([int]$prepared.pid) ([string]$prepared.start_ticks) $gameBase
    $session = [ordered]@{version=1;pid=[int]$prepared.pid;start_ticks=[string]$prepared.start_ticks;game_base=$gameBase;manager=$identity.manager;shape=$identity.shape}
    Invoke-Transaction 'install' $session | Write-Output
    $session | ConvertTo-Json | Set-Content -LiteralPath $activeReceipt -Encoding utf8
    Write-Output "CAMERA_TOOL_INSTALLED active=0 game_camera_natural=1"
    return
}

$session = Assert-Session
if ($Mode -eq 'SetAbsolute') {
    if (-not $AcknowledgeCameraOverrideWrites -or -not $AcknowledgeCrashRisk) {
        throw "camera-write and crash-risk acknowledgements required"
    }
    if ($OverrideFlags -lt 1 -or $OverrideFlags -gt 7) { throw "OverrideFlags must be 1..7" }
    $values = @(
        $OverrideFlags,
        $PositionX.ToString('R',$invariant),$PositionY.ToString('R',$invariant),$PositionZ.ToString('R',$invariant),
        $RotationX.ToString('R',$invariant),$RotationY.ToString('R',$invariant),$RotationZ.ToString('R',$invariant),$RotationW.ToString('R',$invariant),
        $FovRadians.ToString('R',$invariant)
    )
    Invoke-Transaction 'activate' $session $values | Write-Output
    Write-Output "CAMERA_TOOL_ABSOLUTE_ACTIVE override_flags=$OverrideFlags"
} elseif ($Mode -eq 'Disable') {
    Invoke-Transaction 'deactivate' $session | Write-Output
    Write-Output "CAMERA_TOOL_DISABLED game_camera_natural=1 callback_retained=1"
} elseif ($Mode -eq 'Status') {
    Invoke-Transaction 'status' $session | Write-Output
} elseif ($Mode -eq 'Uninstall') {
    if (-not $AcknowledgeBriefWholeProcessStop) { throw "brief-stop acknowledgement required" }
    Invoke-Transaction 'uninstall' $session | Write-Output
    Remove-Item -LiteralPath $activeReceipt -Force
    Assert-CleanTracer ([int]$session.pid)
    Write-Output "CAMERA_TOOL_UNINSTALLED original_callback_restored=1"
}
