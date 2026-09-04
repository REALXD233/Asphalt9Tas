param(
    [ValidateSet("OfflineValidateOnly","PrepareFreshProcess","Install","FreeFlight","Orbital","Disable","Status","Uninstall")]
    [string]$Mode = "OfflineValidateOnly",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [double]$Speed = 30.0,
    [double]$Sensitivity = 0.2,
    [double]$Distance = 5.0,
    [double]$ZoomSpeed = 1.0,
    [double]$FovRadians = 0.959931076,
    [switch]$EnableMouseOnStart,
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
$outDir = Join-Path $root "build\camera-tool-runtime-runner-v2"
$buildDir = Join-Path $root "build\camera-tool-runtime-v2"
$payload = Join-Path $buildDir "liba9tas_camera_tool_runtime_v2_build_only.so"
$controller = Join-Path $buildDir "a9tas_camera_tool_runtime_transaction_v2"
$stream = Join-Path $buildDir "a9tas_camera_tool_runtime_input_stream_v2"
$bootstrap = Join-Path $buildDir "liba9tas_bootstrap_camera_tool_runtime_v2.so"
$managerGraph = Join-Path $root "build\camera-manager-graph-v1\a9tas_camera_manager_graph_check_v1_review_only"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_camera_tool_runtime_preload_v2.sh"
$interactive = Join-Path $root "tools\run_camera_tool_runtime_interactive_v2.py"
$buildScript = Join-Path $root "build-camera-tool-runtime-v2.ps1"
$preparedReceipt = Join-Path $outDir "prepared-process-v2.json"
$activeReceipt = Join-Path $outDir "active-camera-tool-runtime-v2.json"
$pins = @{
    $payload = "68022cb97198c4cfb1573e6c996d23a8e9ceea70e6dab48d621d35398be9c3f2"
    $controller = "0b1ac82beff8f38d7a45af762cae25a43a02542b19068092d39d611ef051e66a"
    $stream = "49a212d730a0365b9a0ff04394c0a7883f2503a3bc2cc1d703a2835d233c1cf3"
    $bootstrap = "a3da7d8040236f416a52e21ad9b0dd9044c9295d369a3fe4d7261c6a6a7c2fe5"
    $managerGraph = "977ea2734fcc8b0e2256f8d533f539d310be9001124f4fd9dc87795cee4f1ef0"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "221830387ba0907c0a798f81089b0c85bea26c79d0d6a1d0fc7529550a6a4ee8"
}
$remotePayload = "/data/local/tmp/liba9tas_camera_tool_runtime_v2_build_only.so"
$remoteController = "/data/local/tmp/a9tas_camera_tool_runtime_transaction_v2"
$remoteStream = "/data/local/tmp/a9tas_camera_tool_runtime_input_stream_v2"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_camera_tool_runtime_v2.so"
$remoteManagerGraph = "/data/local/tmp/a9tas_camera_manager_graph_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_camera_tool_runtime_v2"
$remoteHelper = "/data/local/tmp/run_camera_tool_runtime_preload_v2.sh"
$remoteReport = "/data/local/tmp/a9tas_camera_tool_runtime_report_v2.bin"
$ack = "I_ACCEPT_CAMERA_TOOL_PHASE_ALIGNED_SINGLE_SLOT_V2"
$managerAck = "I_ACCEPT_CAMERA_MANAGER_GRAPH_READ_ONLY_V1"

New-Item -Path $outDir -ItemType Directory -Force | Out-Null

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-LocalArtifacts {
    foreach ($artifact in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
            (Get-Sha256 $artifact) -ne $pins[$artifact]) {
            throw "Pinned Camera Tool v2 artifact mismatch: $artifact"
        }
    }
    if (-not (Test-Path -LiteralPath $interactive -PathType Leaf)) {
        throw "Missing Camera Tool v2 interactive host"
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
    if ($fields.Count -lt 19 -or $fields[18] -notmatch '^\d+$') {
        throw "start ticks parse failed"
    }
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
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "remote hash mismatch: $Path"
    }
}

function Resolve-RaceView([int]$GameProcessId,[string]$StartTicks,[string]$GameBase) {
    $text = Invoke-AdbText @('-s',$Device,'shell',"su -c '$remoteManagerGraph $GameProcessId $GameBase $StartTicks $managerAck'")
    $head = [regex]::Match($text,'(?m)^CAMERA_MANAGER_GRAPH_V1 .*resolve_result=0 graph_result=0 .*manager=0x(?<manager>[0-9a-fA-F]+) ')
    $shapeMatch = [regex]::Match($text,'(?m)^CAMERA_MANAGER_FIELD offset=0xe8 value=0x(?<shape>[0-9a-fA-F]+) ')
    if (-not $head.Success -or -not $shapeMatch.Success) { throw "RaceView resolution failed" }
    [ordered]@{manager=$head.Groups['manager'].Value.ToLowerInvariant();shape=$shapeMatch.Groups['shape'].Value.ToLowerInvariant()}
}

function Assert-Prepared {
    if (-not (Test-Path -LiteralPath $preparedReceipt -PathType Leaf)) {
        throw "prepared receipt missing"
    }
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
    if (-not (Test-Path -LiteralPath $activeReceipt -PathType Leaf)) {
        throw "active Camera Tool v2 receipt missing"
    }
    $session = Get-Content -Raw -LiteralPath $activeReceipt | ConvertFrom-Json
    if ([int]$session.pid -ne [int]$prepared.pid -or
        [string]$session.start_ticks -ne [string]$prepared.start_ticks) {
        throw "active Camera Tool v2 identity mismatch"
    }
    $session
}

function Invoke-Transaction([string]$Action,[object]$Session) {
    Invoke-AdbText @('-s',$Device,'shell',"su -c 'rm -f $remoteReport'") | Out-Null
    $quoted = (@($Action,$Session.pid,$Session.start_ticks,$Session.game_base,
        $Session.manager,$Session.shape,$remoteReport,$ack) -join ' ')
    Invoke-AdbText @('-s',$Device,'shell',"su -c '$remoteController $quoted'")
}

& $buildScript | Write-Host
Assert-LocalArtifacts
python -B (Join-Path $root 'tools\test_camera_tool_runtime_core_v2_policy.py')
if ($LASTEXITCODE -ne 0) { throw "Camera Tool v2 source parity failed" }
if ($Mode -eq 'OfflineValidateOnly') {
    Write-Output "CAMERA_TOOL_RUNTIME_V2_OFFLINE_OK device_access=0 deployed=0 game_writes=0"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB unavailable" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "LDPlayer unavailable"
}

if ($Mode -eq 'PrepareFreshProcess') {
    if (-not $AcknowledgeFreshGameProcessRestart -or
        -not $AcknowledgePreloadWindowInjection) {
        throw "restart and preload acknowledgements required"
    }
    Invoke-AdbText @('-s',$Device,'shell',"am force-stop $package") | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "old process remained alive" }
    foreach ($pair in @(
        @($payload,$remotePayload),@($controller,$remoteController),
        @($stream,$remoteStream),@($bootstrap,$remoteBootstrap),
        @($managerGraph,$remoteManagerGraph),@($injector,$remoteInjector),
        @($helper,$remoteHelper)
    )) { Invoke-AdbText @('-s',$Device,'push',$pair[0],$pair[1]) | Out-Null }
    Invoke-AdbText @('-s',$Device,'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteStream $remoteManagerGraph $remoteInjector $remoteHelper'") | Out-Null
    foreach ($pair in @(
        @($remotePayload,$pins[$payload]),@($remoteController,$pins[$controller]),
        @($remoteStream,$pins[$stream]),@($remoteBootstrap,$pins[$bootstrap]),
        @($remoteManagerGraph,$pins[$managerGraph]),@($remoteInjector,$pins[$injector]),
        @($remoteHelper,$pins[$helper])
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
    [ordered]@{version=2;device=$Device;pid=$gameProcessId;start_ticks=(Get-StartTicks $gameProcessId)} |
        ConvertTo-Json | Set-Content -LiteralPath $preparedReceipt -Encoding utf8
    Remove-Item -LiteralPath $activeReceipt -Force -ErrorAction SilentlyContinue
    Write-Output "CAMERA_TOOL_RUNTIME_V2_PREPARED pid=$gameProcessId callback_installed=0 game_writes=0"
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
    $session = [ordered]@{version=2;pid=[int]$prepared.pid;start_ticks=[string]$prepared.start_ticks;game_base=$gameBase;manager=$identity.manager;shape=$identity.shape}
    $text = Invoke-Transaction 'install' $session
    $proof = [regex]::Match($text,'(?m)^CAMERA_TOOL_RUNTIME_TRANSACTION action=1 flags=0x(?<flags>[0-9a-f]+) .* control=0x(?<control>[0-9a-f]+) evidence=0x(?<evidence>[0-9a-f]+) ')
    if (-not $proof.Success -or
        (([Convert]::ToUInt32($proof.Groups['flags'].Value,16) -band 0x37f) -ne 0x37f)) {
        throw "Camera Tool v2 install proof missing: $text"
    }
    $session.control = $proof.Groups['control'].Value.ToLowerInvariant()
    $session.evidence = $proof.Groups['evidence'].Value.ToLowerInvariant()
    $session | ConvertTo-Json | Set-Content -LiteralPath $activeReceipt -Encoding utf8
    Write-Output $text
    Write-Output "CAMERA_TOOL_RUNTIME_V2_INSTALLED active=0 phase_aligned=1"
    return
}

$session = Assert-Session
if ($Mode -in @('FreeFlight','Orbital')) {
    if (-not $AcknowledgeCameraOverrideWrites -or -not $AcknowledgeCrashRisk) {
        throw "camera-write and crash-risk acknowledgements required"
    }
    $modeName = if ($Mode -eq 'FreeFlight') { 'free' } else { 'orbital' }
    $arguments = @('-B',$interactive,'--mode',$modeName,'--session',$activeReceipt,
        '--adb',$AdbPath,'--device',$Device,'--speed',$Speed,
        '--sensitivity',$Sensitivity,'--distance',$Distance,
        '--zoom-speed',$ZoomSpeed,'--fov',$FovRadians)
    if ($EnableMouseOnStart) { $arguments += '--enable-mouse-on-start' }
    python @arguments
    if ($LASTEXITCODE -ne 0) { throw "Camera Tool v2 interactive host failed" }
} elseif ($Mode -eq 'Disable') {
    Invoke-Transaction 'deactivate' $session | Write-Output
    Write-Output "CAMERA_TOOL_RUNTIME_V2_DISABLED game_camera_natural=1 callback_retained=1"
} elseif ($Mode -eq 'Status') {
    Invoke-Transaction 'status' $session | Write-Output
} elseif ($Mode -eq 'Uninstall') {
    if (-not $AcknowledgeBriefWholeProcessStop) { throw "brief-stop acknowledgement required" }
    Invoke-Transaction 'uninstall' $session | Write-Output
    Remove-Item -LiteralPath $activeReceipt -Force
    Assert-CleanTracer ([int]$session.pid)
    Write-Output "CAMERA_TOOL_RUNTIME_V2_UNINSTALLED original_callback_restored=1"
}
