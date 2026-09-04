# Guarded composite replay: lifecycle-bound final writer plus AluTasV2-style
# per-frame natural action counts. Default mode is strictly offline.

param(
    [ValidateSet("OfflineValidate", "PrepareFreshProcess", "ExecuteComposite")]
    [string]$Mode = "OfflineValidate",
    [ValidateRange(30000, 180000)][int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ObjectHex = "",
    [string]$StateAddressHex = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [string]$OutputDirectory = "",
    [switch]$AcknowledgeFreshProcessPreload,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeThreeAutomaticEscapeTransitions,
    [switch]$Acknowledge900FrameCompositeWrites,
    [switch]$AcknowledgeNoManualInputDuringReplay,
    [switch]$AcknowledgeFailureForceStopsFreshProcess,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk,
    [switch]$ExecuteExactlyOneAttempt,
    [switch]$EnablePhysicsIntervalRecord,
    [switch]$Acknowledge900PhysicsIntervalRecordCalls,
    [switch]$EnablePhysicsIntervalReplay,
    [string]$PhysicsIntervalReplaySource = "",
    [switch]$Acknowledge900PhysicsIntervalReplayCalls
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$buildScript = Join-Path $root "build-final-writer-natural-action-live-v1.ps1"
$writerPayload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$actionPayload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$bootstrap = Join-Path $root "build\final-writer-natural-action-live-v1\liba9tas_bootstrap_final_writer_natural_action_v1.so"
$writerCandidate = Join-Path $root "build\lifecycle-final-writer-natural-action-v1\a9tas_lifecycle_final_writer_natural_action_v1_review_only"
$actionController = Join-Path $root "build\natural-action-external-replay-controller-v1\natural_action_external_replay_controller_v1_review_only"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_final_writer_natural_action_preload_v1.sh"
$recording = Join-Path $root "evidence\a9tas_lifecycle_source_900f_20260822_121419_985.a9utk1"
$target = Join-Path $root "evidence\a9tas_lifecycle_natural_900f_20260822_121419_985.a9fwt1"
$parser = Join-Path $root "tools\parse_unified_executor_report_v8.py"
$pairValidator = Join-Path $root "tools\validate_final_writer_report_pair_v1.py"
$actionValidator = Join-Path $root "tools\validate_natural_action_external_replay_report_v1.py"
$policy = Join-Path $root "tools\test_run_final_writer_natural_action_composite_policy_v1.py"
$tripleBuildScript = Join-Path $root "build-final-writer-natural-action-physics-interval-live-v1.ps1"
$tripleBootstrap = Join-Path $root "build\final-writer-natural-action-physics-interval-live-v1\liba9tas_bootstrap_final_writer_natural_action_physics_interval_v1.so"
$tripleHelper = Join-Path $root "tools\run_final_writer_natural_action_physics_interval_preload_v1.sh"
$intervalPayload = Join-Path $root "build\physics-interval-getter-payload-v2\liba9tas_physics_interval_getter_v2_passive.so"
$intervalController = Join-Path $root "build\physics-interval-getter-shadow-controller-v2\a9tas_physics_interval_shadow_controller_v2"
$intervalObserver = Join-Path $root "build\physics-interval-readonly-v1\a9tas_physics_interval_readonly_observer_v1"
$intervalObjectParser = Join-Path $root "tools\parse_physics_interval_readonly_v1.py"
$intervalReceiptParser = Join-Path $root "tools\parse_physics_interval_shadow_receipt_v2.py"
$intervalAck = "I_ACCEPT_PHYSICS_INTERVAL_SHADOW_V2"
$intervalLimit = 900
$intervalReplayDefault = Join-Path $root "evidence\a9tas_composite_900f_20260823_105723_425.a9pgtr2.intervals.bin"
$intervalReplayExpectedHash = "329683a976ad7d0e3f7d326c567fbdfaf911be1fc6bca5573087de2e11bf6347"
if ($EnablePhysicsIntervalRecord -and $EnablePhysicsIntervalReplay) {
    throw "Physics Interval record and replay modes are mutually exclusive"
}
$physicsIntervalEnabled = [bool]($EnablePhysicsIntervalRecord -or
                                 $EnablePhysicsIntervalReplay)
$intervalMode = if ($EnablePhysicsIntervalRecord) {
    "record"
} elseif ($EnablePhysicsIntervalReplay) {
    "replay"
} else {
    "none"
}
if ($EnablePhysicsIntervalReplay) {
    if ($PhysicsIntervalReplaySource -eq "") {
        $PhysicsIntervalReplaySource = $intervalReplayDefault
    }
    $PhysicsIntervalReplaySource = [IO.Path]::GetFullPath(
        $PhysicsIntervalReplaySource)
}
if ($physicsIntervalEnabled) {
    $bootstrap = $tripleBootstrap
    $helper = $tripleHelper
}
$receiptName = if ($physicsIntervalEnabled) {
    "prepared-process-physics-interval-$intervalMode-v1.json"
} else {
    "prepared-process-v1.json"
}
$receipt = Join-Path $root "build\final-writer-natural-action-live-v1\$receiptName"

$pins = @{
    $writerPayload = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
    $actionPayload = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
    $bootstrap = if ($physicsIntervalEnabled) {
        "e87f2f8a7aca73f3d3cf83b7a8af6be51de1ccbdf5c4eb60e387354362e09cf5"
    } else {
        "d4f2d4639aea8c3369bb9ca514715c96a46f50de236943265b514d072cc65646"
    }
    $writerCandidate = "7ac3464bac63fe495a07c5e74f495099fe45613e0c573f32abdae12a14520f96"
    $actionController = "7a1659ef56bdd7cad3591ad06ab533b1fa72f31adc1598fec1b8e4c7958a2138"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $recording = "727e850fac73cdcf69d5eff1684f946dd129b542e72cf7d34a67fd379d47229b"
    $target = "1daf77e1608ad5880192bcd052be936962295f271314f0b98465eaaa80682970"
}

$remoteWriterPayload = "/data/local/tmp/liba9tas_final_writer_replay_v1_build_only.so"
$remoteActionPayload = "/data/local/tmp/liba9tas_natural_action_replay_v1_review_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_natural_action_v1.so"
$remoteWriter = "/data/local/tmp/a9tas_lifecycle_final_writer_natural_action_v1"
$remoteAction = "/data/local/tmp/a9tas_natural_action_external_replay_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_final_writer_natural_action_v1"
$remoteHelper = "/data/local/tmp/run_final_writer_natural_action_preload_v1.sh"
$remoteIntervalPayload = "/data/local/tmp/liba9tas_physics_interval_getter_v2_passive.so"
$remoteIntervalController = "/data/local/tmp/a9tas_physics_interval_shadow_controller_v2"
$remoteIntervalObserver = "/data/local/tmp/a9tas_physics_interval_readonly_observer_v1"
if ($physicsIntervalEnabled) {
    $remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_natural_action_physics_interval_v1.so"
    $remoteInjector = "/data/local/tmp/a9tas_injector_final_writer_natural_action_physics_interval_v1"
    $remoteHelper = "/data/local/tmp/run_final_writer_natural_action_physics_interval_preload_v1.sh"
    $pins[$intervalPayload] = "f49b78eff51b606436f291c837ab50c297286ae3f43a10352dc2aa77ca8a62df"
    $pins[$intervalController] = "1e1aa77a35fdfd596cc572f98fc99090bd66cc188bdb15b155705e58feee56a0"
    $pins[$intervalObserver] = "e8e75ac176650ba48962774a8ad6a209d00f27592d09c935e739722c785a8005"
}
$writerAck = "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1"
$actionAck = "I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1"

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
function Get-GamePid {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'pidof $package'")
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Get-StartTicks([int]$GamePid) {
    $stat = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed process stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start ticks unavailable"
    }
    return [string]$fields[19]
}
function Get-TracerPid([int]$GamePid) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($line -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid" }
    return [int]$matches[1]
}
function Get-GameBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $values = @(
        @(foreach ($line in $maps) {
            if ($line -match 'libAsphalt9\.so' -and
                $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
                [Convert]::ToUInt64($matches[2], 16) -eq 0) {
                $matches[1].ToLowerInvariant()
            }
        }) | Sort-Object -Unique
    )
    if ($values.Count -ne 1) { throw "Expected one game library base" }
    return [string]$values[0]
}
function Normalize-Hex([string]$Value, [string]$Label) {
    $result = $Value.Trim()
    if ($result.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
        $result = $result.Substring(2)
    }
    if ($result -notmatch '^[0-9a-fA-F]{8,16}$') { throw "$Label is invalid" }
    return $result.ToLowerInvariant()
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Path'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Path"
    }
}
function Wait-RemoteFile([string]$Path, [int]$Milliseconds) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $Path; echo `$?'")) -eq '0') {
            return $true
        }
        Start-Sleep -Milliseconds 25
    }
    return $false
}
function Start-Remote([string]$Command) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $AdbPath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in @('-s', $Device, 'shell', "su -c '$Command'")) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "Failed to start remote candidate" }
    return [pscustomobject]@{
        Process = $process
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
    }
}
function Read-RemoteResult($Handle) {
    return [pscustomobject]@{
        ExitCode = $Handle.Process.ExitCode
        Stdout = $Handle.Stdout.GetAwaiter().GetResult()
        Stderr = $Handle.Stderr.GetAwaiter().GetResult()
    }
}

$requiredInputs = @($buildScript, $writerPayload, $actionPayload, $bootstrap,
                    $writerCandidate, $actionController, $injector, $helper,
                    $recording, $target, $parser, $pairValidator,
                    $actionValidator, $policy)
if ($physicsIntervalEnabled) {
    $requiredInputs += @($tripleBuildScript, $intervalPayload,
                         $intervalController, $intervalObserver,
                         $intervalObjectParser, $intervalReceiptParser)
}
if ($EnablePhysicsIntervalReplay) {
    $requiredInputs += @($PhysicsIntervalReplaySource)
}
foreach ($path in $requiredInputs) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing composite input: $path"
    }
}
foreach ($path in $pins.Keys) {
    if ((Get-Sha $path) -ne $pins[$path]) { throw "Composite pin mismatch: $path" }
}
if ($EnablePhysicsIntervalReplay -and
    (Get-Sha $PhysicsIntervalReplaySource) -ne
        $intervalReplayExpectedHash) {
    throw "Exact 900-call Physics Interval replay source hash mismatch"
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "Composite runner policy failed" }
if ($Mode -eq 'OfflineValidate') {
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Composite build validation failed" }
    if ($physicsIntervalEnabled) {
        & $tripleBuildScript | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Triple preload build validation failed" }
    }
    foreach ($path in $pins.Keys) {
        if ((Get-Sha $path) -ne $pins[$path]) { throw "Post-build pin mismatch: $path" }
    }
    $intervalText = if ($EnablePhysicsIntervalRecord) {
        " physics_interval_record=900 triple_preload=1"
    } elseif ($EnablePhysicsIntervalReplay) {
        " physics_interval_replay=900 source_sha256=$intervalReplayExpectedHash triple_preload=1"
    } else { "" }
    Write-Output "FINAL_WRITER_NATURAL_ACTION_COMPOSITE_OFFLINE passed=1 frames=900 source=recorded_natural activation_frame=430 action_calls=1$intervalText deployed=0 device_access=0"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}

if ($Mode -eq 'PrepareFreshProcess') {
    if (-not $AcknowledgeFreshProcessPreload) { throw "Must acknowledge paired preload" }
    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") 'force-stop old game' | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Composite build failed" }
    if ($physicsIntervalEnabled) {
        & $tripleBuildScript | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Triple preload build failed" }
    }
    $preloadPairs = @(
        @($writerPayload, $remoteWriterPayload),
        @($actionPayload, $remoteActionPayload),
        @($bootstrap, $remoteBootstrap),
        @($injector, $remoteInjector),
        @($helper, $remoteHelper)
    )
    if ($physicsIntervalEnabled) {
        $preloadPairs += ,@($intervalPayload, $remoteIntervalPayload)
        $preloadPairs += ,@($intervalController, $remoteIntervalController)
        $preloadPairs += ,@($intervalObserver, $remoteIntervalObserver)
    }
    foreach ($pair in $preloadPairs) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push preload artifact' | Out-Null
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remoteWriterPayload $remoteActionPayload $remoteBootstrap; chmod 700 $remoteInjector $remoteHelper'") 'chmod preload artifacts' | Out-Null
    if ($physicsIntervalEnabled) {
        Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c 'chmod 644 $remoteIntervalPayload; chmod 700 $remoteIntervalController $remoteIntervalObserver'") 'chmod interval artifacts' | Out-Null
    }
    $preloadOut = Join-Path (Split-Path -Parent $receipt) 'preload.stdout.txt'
    $preloadErr = Join-Path (Split-Path -Parent $receipt) 'preload.stderr.txt'
    $preload = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $preloadOut `
        -RedirectStandardError $preloadErr
    Start-Sleep -Milliseconds 300
    Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") 'start fresh game' | Out-Null
    if (-not $preload.WaitForExit(45000)) {
        Stop-Process -Id $preload.Id -Force
        throw "Composite preload timed out"
    }
    $preloadText = (Get-Content -Raw $preloadOut) + "`n" + (Get-Content -Raw $preloadErr)
    if ($preload.ExitCode -ne 0 -or $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "Composite preload did not reach armed stage: $preloadText"
    }
    $gamePid = Get-GamePid
    if ($gamePid -le 0) { throw "Fresh game PID unavailable" }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $mapped = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
        $mapped = $maps.Contains($remoteWriterPayload) -and
                  $maps.Contains($remoteActionPayload) -and
                  $maps.Contains($remoteBootstrap)
        if ($physicsIntervalEnabled) {
            $mapped = $mapped -and $maps.Contains($remoteIntervalPayload)
        }
        if ($mapped) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped -or (Get-TracerPid $gamePid) -ne 0) {
        throw "Paired payload mapping or tracer proof failed"
    }
    $startTicks = Get-StartTicks $gamePid
    [ordered]@{
        version=1; device=$Device; pid=$gamePid; start_ticks=$startTicks;
        writer_payload_sha256=$pins[$writerPayload];
        action_payload_sha256=$pins[$actionPayload];
        bootstrap_sha256=$pins[$bootstrap];
        physics_interval_enabled=$physicsIntervalEnabled;
        physics_interval_mode=$intervalMode;
        physics_interval_payload_sha256=if ($physicsIntervalEnabled) {
            $pins[$intervalPayload]
        } else { "" }
        physics_interval_source_sha256=if ($EnablePhysicsIntervalReplay) {
            $intervalReplayExpectedHash
        } else { "" }
    } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    $payloadCount = if ($physicsIntervalEnabled) { 3 } else { 2 }
    Write-Output "FINAL_WRITER_NATURAL_ACTION_PREPARE_PASSED pid=$gamePid start_ticks=$startTicks payloads=$payloadCount TracerPid=0"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneAttempt, 'exactly one attempt'),
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, 'Ancient Ruins + ZL1 countdown-3 paused'),
    @($AcknowledgeThreeAutomaticEscapeTransitions, 'automatic resume, re-pause, and final resume ESC transitions'),
    @($Acknowledge900FrameCompositeWrites, '900 fixed-delta/control/final-writer frames plus 900 natural-action commands'),
    @($AcknowledgeNoManualInputDuringReplay, 'no manual input during replay'),
    @($AcknowledgeFailureForceStopsFreshProcess, 'fresh process force-stop on uncertain failure'),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, 'ptrace stall, rollback, and crash risk')
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if ($EnablePhysicsIntervalRecord -and
    -not $Acknowledge900PhysicsIntervalRecordCalls) {
    throw "Must acknowledge 900 source-faithful Physics Interval record calls"
}
if ($EnablePhysicsIntervalReplay -and
    -not $Acknowledge900PhysicsIntervalReplayCalls) {
    throw "Must acknowledge 900 source-faithful Physics Interval replay overrides"
}
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared-process receipt missing" }

$object = Normalize-Hex $ObjectHex 'ObjectHex'
$state = Normalize-Hex $StateAddressHex 'StateAddressHex'
if ([Convert]::ToUInt64($state, 16) -ne
    [Convert]::ToUInt64($object, 16) + 0x2d8) {
    throw "StateAddressHex must equal ObjectHex + 0x2D8"
}
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid optional address" }
}

$prepared = Get-Content -Raw $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
$startTicks = if ($gamePid -gt 0) { Get-StartTicks $gamePid } else { '' }
if ($prepared.version -ne 1 -or $prepared.device -ne $Device -or
    [int]$prepared.pid -ne $gamePid -or
    [string]$prepared.start_ticks -ne $startTicks -or
    $prepared.writer_payload_sha256 -ne $pins[$writerPayload] -or
    $prepared.action_payload_sha256 -ne $pins[$actionPayload] -or
    $prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or
    [bool]$prepared.physics_interval_enabled -ne
        $physicsIntervalEnabled -or
    ($physicsIntervalEnabled -and
     [string]$prepared.physics_interval_mode -ne $intervalMode) -or
    ($physicsIntervalEnabled -and
     $prepared.physics_interval_payload_sha256 -ne $pins[$intervalPayload]) -or
    ($EnablePhysicsIntervalReplay -and
     $prepared.physics_interval_source_sha256 -ne
        $intervalReplayExpectedHash) -or
    (Get-TracerPid $gamePid) -ne 0) {
    throw "Prepared process identity mismatch"
}
$maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
if (-not $maps.Contains($remoteWriterPayload) -or
    -not $maps.Contains($remoteActionPayload) -or
    -not $maps.Contains($remoteBootstrap) -or
    ($physicsIntervalEnabled -and
     -not $maps.Contains($remoteIntervalPayload))) {
    throw "Prepared paired mappings missing"
}
$base = Get-GameBaseHex $gamePid
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
if ($OutputDirectory -eq '') { $OutputDirectory = Join-Path $root 'evidence' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$remoteRecording = "/data/local/tmp/a9tas_composite_${gamePid}_$stamp.a9utk1"
$remoteTarget = "/data/local/tmp/a9tas_composite_${gamePid}_$stamp.a9fwt1"
$remoteUnified = "/data/local/tmp/a9tas_composite_${gamePid}_$stamp.a9uer8"
$remoteWriterReport = "/data/local/tmp/a9tas_composite_${gamePid}_$stamp.a9fwr1"
$remoteActionReport = "/data/local/tmp/a9tas_composite_${gamePid}_$stamp.a9nar6"
$nalReady = "/data/local/tmp/a9tas_composite_nal_ready_${gamePid}_$stamp"
$nalArm = "/data/local/tmp/a9tas_composite_nal_arm_${gamePid}_$stamp"
$externalReady = "/data/local/tmp/a9tas_composite_external_${gamePid}_$stamp"
$writerReady = "/data/local/tmp/a9tas_final_writer_ready_$gamePid"
$localUnified = Join-Path $OutputDirectory "a9tas_composite_900f_$stamp.a9uer8"
$localWriterReport = Join-Path $OutputDirectory "a9tas_composite_900f_$stamp.a9fwr1"
$localActionReport = Join-Path $OutputDirectory "a9tas_composite_900f_$stamp.a9nar6"
$remoteIntervalPrefix = "/data/local/tmp/a9tas_composite_interval_${gamePid}_$stamp"
$remoteIntervalInput = "$remoteIntervalPrefix.intervals.bin"
$localIntervalObservation = Join-Path $OutputDirectory "a9tas_composite_900f_$stamp.a9pio2"
$localIntervalReceipt = Join-Path $OutputDirectory "a9tas_composite_900f_$stamp.a9pgtr2"

$mutationStarted = $false
$success = $false
$actionHandle = $null
$writerHandle = $null
$intervalInstalled = $false
$intervalFinalized = $false
$intervalObject = ""
try {
    foreach ($pair in @(
        @($writerCandidate, $remoteWriter, $pins[$writerCandidate]),
        @($actionController, $remoteAction, $pins[$actionController]),
        @($recording, $remoteRecording, $pins[$recording]),
        @($target, $remoteTarget, $pins[$target])
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push composite artifact' | Out-Null
        Assert-RemoteHash $pair[1] $pair[2]
    }
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteWriter $remoteAction'") 'chmod composite candidates' | Out-Null
    if ($physicsIntervalEnabled) {
        Assert-RemoteHash $remoteIntervalPayload $pins[$intervalPayload]
        Assert-RemoteHash $remoteIntervalController $pins[$intervalController]
        Assert-RemoteHash $remoteIntervalObserver $pins[$intervalObserver]
        $entryCommand = "$remoteIntervalController status 2147483646 1 1000 1000 $intervalMode $intervalLimit - $remoteIntervalPrefix.entrypoint.a9pgtr2 $intervalAck"
        $entryResult = & $AdbPath -s $Device shell "su -c '$entryCommand'" 2>&1
        $entryExit = $LASTEXITCODE
        if ($entryExit -ne 3 -or
            ($entryResult -join "`n") -notmatch 'stage=process_identity') {
            throw "Physics Interval controller entrypoint preflight failed"
        }
        if ($EnablePhysicsIntervalReplay) {
            Invoke-AdbChecked @('-s', $Device, 'push',
                $PhysicsIntervalReplaySource, $remoteIntervalInput) `
                'push exact Physics Interval replay source' | Out-Null
            Assert-RemoteHash $remoteIntervalInput $intervalReplayExpectedHash
            # Install parses the exact replay stream before process identity.
            # An impossible PID proves parsing without touching the game.
            $inputProbeOutput = "$remoteIntervalPrefix.input_entrypoint.a9pgtr2"
            $inputProbeCommand = "$remoteIntervalController install 2147483646 1 1000 1000 replay $intervalLimit $remoteIntervalInput $inputProbeOutput $intervalAck"
            $inputProbeResult = & $AdbPath -s $Device shell `
                "su -c '$inputProbeCommand'" 2>&1
            $inputProbeExit = $LASTEXITCODE
            if ($inputProbeExit -ne 3 -or
                ($inputProbeResult -join "`n") -notmatch
                    'stage=process_identity') {
                throw "Physics Interval replay-source entrypoint preflight failed"
            }
        }
    }
    $actionCommand = "$remoteAction $gamePid $startTicks $base $TimeoutMs $remoteActionReport $nalReady $nalArm $externalReady 900 $actionAck"
    $actionHandle = Start-Remote $actionCommand
    if (-not (Wait-RemoteFile $nalReady 7000)) { throw "Natural-action registration did not reach READY_NO_ATTACH" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $nalReady'")
    if ($readyText -notmatch '^NAL_READY_NO_ATTACH ' -or
        $readyText -notmatch 'paused_zero_samples=8 target_threads_attached=0 game_writes=0') {
        throw "Natural-action READY proof mismatch"
    }
    $mutationStarted = $true
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'input keyevent 111 && echo NAL_ARM $gamePid $startTicks > $nalArm'") 'resume and arm natural registration' | Out-Null
    if (-not (Wait-RemoteFile $externalReady 15000)) { throw "Persistent action callback was not registered" }
    if ((Get-TracerPid $gamePid) -ne 0) { throw "Natural controller did not detach before handoff" }
    $externalText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $externalReady'")
    if ($externalText -notmatch '^NAL_EXTERNAL_REPLAY_READY ' -or
        $externalText -notmatch 'frames=900 tracer=0 published=0 completed=0$') {
        throw "Empty-mailbox registration proof mismatch"
    }
    # Re-pause immediately while the authoritative lifecycle is still state 2.
    Invoke-AdbChecked @('-s', $Device, 'shell', "input keyevent 111") 're-pause after callback registration' | Out-Null
    Start-Sleep -Milliseconds 250

    if ($physicsIntervalEnabled) {
        if ((Get-GamePid) -ne $gamePid -or
            (Get-StartTicks $gamePid) -ne $startTicks -or
            (Get-TracerPid $gamePid) -ne 0) {
            throw "Physics Interval pre-install process identity failed"
        }
        $remoteIntervalObservation = "$remoteIntervalPrefix.a9pio2"
        $observationCommand = "$remoteIntervalObserver $gamePid $base 300 10 $remoteIntervalObservation"
        Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c '$observationCommand'") 'locate Physics Interval object' | Out-Host
        Invoke-AdbChecked @('-s', $Device, 'pull', $remoteIntervalObservation,
            $localIntervalObservation) 'pull Physics Interval observation' | Out-Null
        $intervalJson = (& python -B $intervalObjectParser `
            $localIntervalObservation --json --profile car-physics) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "Physics Interval object profile failed" }
        $intervalObservation = $intervalJson | ConvertFrom-Json
        if (-not $intervalObservation.passed -or
            $intervalObservation.step_options.Count -ne 1) {
            throw "Physics Interval object identity is not unique"
        }
        $intervalObject = Normalize-Hex `
            ([string]$intervalObservation.step_options[0]) `
            'PhysicsIntervalStepOptions'
        $preflightOutput = "$remoteIntervalPrefix.preflight.a9pgtr2"
        $preflightCommand = "$remoteIntervalController preflight $gamePid $startTicks $base $intervalObject $intervalMode $intervalLimit - $preflightOutput $intervalAck"
        $preflightText = (Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c '$preflightCommand'") 'Physics Interval read-only preflight') -join "`n"
        if ($preflightText -notmatch 'PHYSICS_INTERVAL_SHADOW_PREFLIGHT' -or
            $preflightText -notmatch 'game_writes=0') {
            throw "Physics Interval read-only preflight proof failed"
        }
        $installOutput = "$remoteIntervalPrefix.install.a9pgtr2"
        $intervalInstallInput = if ($EnablePhysicsIntervalReplay) {
            $remoteIntervalInput
        } else {
            "-"
        }
        $installCommand = "$remoteIntervalController install $gamePid $startTicks $base $intervalObject $intervalMode $intervalLimit $intervalInstallInput $installOutput $intervalAck"
        $intervalInstalled = $true
        $installText = (Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c '$installCommand'") "install Physics Interval $intervalMode hook") -join "`n"
        if ($installText -notmatch 'PHYSICS_INTERVAL_SHADOW_TRANSACTION action=1' -or
            $installText -notmatch 'game_writes=1') {
            throw "Physics Interval install proof failed"
        }
    }

    $writerCommand = "$remoteWriter $gamePid $base $TimeoutMs $startTicks $remoteRecording $remoteTarget $remoteUnified $remoteWriterReport $writerAck $object $state $PhysicsContextHex $MainObjectHex $FinalOwnerHex"
    $writerHandle = Start-Remote $writerCommand
    if (-not (Wait-RemoteFile $writerReady 15000)) { throw "Composite final writer did not reach armed READY" }
    $writerReadyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $writerReady'")
    if ($writerReadyText -notmatch '^READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1 ' -or
        $writerReadyText -notmatch 'state=2 .*all_target_threads_frozen=1 host_resume_gate=marker_removal$') {
        throw "Composite final-writer READY proof mismatch"
    }
    if ($writerReadyText -notmatch 'controller_pid=(\d+)') { throw "Writer tracer PID missing" }
    $writerTracer = [int]$matches[1]
    if ((Get-TracerPid $gamePid) -ne $writerTracer) { throw "Writer tracer identity mismatch" }
    $esc = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'input keyevent 111') `
        -WindowStyle Hidden -PassThru
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $writerReady'") 'release composite writer' | Out-Null
    if (-not $esc.WaitForExit(10000) -or $esc.ExitCode -ne 0) { throw "Final resume ESC failed" }
    if (-not $writerHandle.Process.WaitForExit($TimeoutMs + 15000)) { throw "Composite final writer timed out" }
    $writerResult = Read-RemoteResult $writerHandle
    if ($writerResult.Stdout) { $writerResult.Stdout.TrimEnd() | Write-Host }
    if ($writerResult.Stderr) { $writerResult.Stderr.TrimEnd() | Write-Warning }
    if ($writerResult.ExitCode -ne 0 -or
        $writerResult.Stdout -notmatch 'AUTHORITATIVE_RACE_START_V1 state=3 events=1') {
        throw "Composite final writer failed closed"
    }
    if ((Get-TracerPid $gamePid) -ne 0) { throw "Writer tracer did not detach" }
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $externalReady'") 'release natural cleanup' | Out-Null
    if (-not $actionHandle.Process.WaitForExit(15000)) { throw "Natural callback cleanup timed out" }
    $actionResult = Read-RemoteResult $actionHandle
    if ($actionResult.Stdout) { $actionResult.Stdout.TrimEnd() | Write-Host }
    if ($actionResult.Stderr) { $actionResult.Stderr.TrimEnd() | Write-Warning }
    if ($actionResult.ExitCode -ne 0 -or $actionResult.Stdout -notmatch 'NAL_DONE success=1') {
        throw "Natural callback cleanup/report failed"
    }
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
        (Get-TracerPid $gamePid) -ne 0) {
        throw "Composite final process identity failed"
    }
    if ($physicsIntervalEnabled) {
        $statusOutput = "$remoteIntervalPrefix.status.a9pgtr2"
        $statusCommand = "$remoteIntervalController status $gamePid $startTicks $base $intervalObject $intervalMode $intervalLimit - $statusOutput $intervalAck"
        $statusText = (Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c '$statusCommand'") 'read Physics Interval completion status') -join "`n"
        if ($statusText -notmatch 'PHYSICS_INTERVAL_SHADOW_STATUS complete=1\b' -or
            $statusText -notmatch 'cursor=900\b' -or
            $statusText -notmatch 'calls=900\b' -or
            $statusText -notmatch 'active=0\b') {
            throw "Physics Interval 900-call $intervalMode did not complete exactly"
        }
        $finalOutput = "$remoteIntervalPrefix.final.a9pgtr2"
        $finalizeCommand = "$remoteIntervalController finalize $gamePid $startTicks $base $intervalObject $intervalMode $intervalLimit - $finalOutput $intervalAck"
        $finalizeText = (Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c '$finalizeCommand'") "finalize Physics Interval $intervalMode") -join "`n"
        if ($finalizeText -notmatch 'PHYSICS_INTERVAL_SHADOW_TRANSACTION action=3' -or
            $finalizeText -notmatch 'game_writes=1') {
            throw "Physics Interval finalize/restore proof failed"
        }
        $intervalFinalized = $true
        Invoke-AdbChecked @('-s', $Device, 'pull', $finalOutput,
            $localIntervalReceipt) 'pull Physics Interval receipt' | Out-Null
        $intervalJsonPath = "$localIntervalReceipt.analysis.json"
        $intervalBitsPath = "$localIntervalReceipt.intervals.bin"
        & python -B $intervalReceiptParser $localIntervalReceipt --json `
            $intervalJsonPath --extract-intervals $intervalBitsPath
        if ($LASTEXITCODE -ne 0) {
            throw "Physics Interval receipt parser rejected the 900-call recording"
        }
        $intervalAnalysis = Get-Content -LiteralPath $intervalJsonPath -Raw |
            ConvertFrom-Json
        $expectedIntervalModeNumber = if ($EnablePhysicsIntervalReplay) {
            2
        } else {
            1
        }
        $expectedRecordCalls = if ($EnablePhysicsIntervalRecord) {
            $intervalLimit
        } else {
            0
        }
        $expectedReplayCalls = if ($EnablePhysicsIntervalReplay) {
            $intervalLimit
        } else {
            0
        }
        $expectedOverrides = $expectedReplayCalls
        $intervalEventFailures = 0
        if (@($intervalAnalysis.intervals).Count -eq $intervalLimit -and
            @($intervalAnalysis.events).Count -eq $intervalLimit) {
            for ($intervalIndex = 0; $intervalIndex -lt $intervalLimit;
                 ++$intervalIndex) {
                $intervalEntry = $intervalAnalysis.intervals[$intervalIndex]
                $intervalEvent = $intervalAnalysis.events[$intervalIndex]
                $eventValid = $intervalEvent.sequence -eq $intervalIndex -and
                    $intervalEvent.commit_sequence -eq
                        ($intervalEvent.sequence + 1)
                if ($EnablePhysicsIntervalReplay) {
                    $eventValid = $eventValid -and
                        $intervalEvent.flags -eq "0x3f" -and
                        $intervalEvent.requested_bits -eq $intervalEntry.bits -and
                        $intervalEvent.final_bits -eq $intervalEntry.bits
                } else {
                    $eventValid = $eventValid -and
                        $intervalEvent.flags -eq "0x33" -and
                        $intervalEvent.requested_bits -eq "0x00000000" -and
                        $intervalEvent.original_bits -eq
                            $intervalEvent.final_bits
                }
                if (-not $eventValid) { ++$intervalEventFailures }
            }
        } else {
            $intervalEventFailures = 1
        }
        if (-not $intervalAnalysis.complete -or
            $intervalAnalysis.mode -ne $expectedIntervalModeNumber -or
            $intervalAnalysis.evidence.semantic_errors -ne 0 -or
            $intervalAnalysis.evidence.object_mismatches -ne 0 -or
            $intervalAnalysis.evidence.tid_changes -ne 0 -or
            $intervalAnalysis.evidence.record_calls -ne $expectedRecordCalls -or
            $intervalAnalysis.evidence.replay_calls -ne $expectedReplayCalls -or
            $intervalAnalysis.evidence.overrides -ne $expectedOverrides -or
            @($intervalAnalysis.intervals).Count -ne $intervalLimit -or
            @($intervalAnalysis.events).Count -ne $intervalLimit -or
            @($intervalAnalysis.intervals | Where-Object { -not $_.valid }).Count -ne 0 -or
            $intervalEventFailures -ne 0) {
            throw "Physics Interval 900-call evidence failed strict semantic checks"
        }
        if ($EnablePhysicsIntervalReplay -and
            (Get-Sha $intervalBitsPath) -ne $intervalReplayExpectedHash) {
            throw "Physics Interval replay receipt does not match the exact pinned source stream"
        }
        if ((Get-TracerPid $gamePid) -ne 0) {
            throw "Physics Interval finalize left a tracer attached"
        }
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remoteUnified $remoteWriterReport $remoteActionReport'") 'chmod composite reports' | Out-Null
    foreach ($pair in @(
        @($remoteUnified, $localUnified),
        @($remoteWriterReport, $localWriterReport),
        @($remoteActionReport, $localActionReport)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'pull', $pair[0], $pair[1]) 'pull composite report' | Out-Null
    }
    python -B $parser $localUnified
    if ($LASTEXITCODE -ne 0) { throw "Composite A9UER8 validation failed" }
    python -B $pairValidator $localUnified $localWriterReport $target $writerPayload
    if ($LASTEXITCODE -ne 0) { throw "Composite final-writer pair validation failed" }
    python -B $actionValidator $localActionReport $recording
    if ($LASTEXITCODE -ne 0) { throw "Composite natural-action report validation failed" }
    Remove-Item -LiteralPath $receipt -Force
    $success = $true
    $intervalPassText = if ($EnablePhysicsIntervalReplay) {
        " physics_interval_replay=900 overrides=900 exact_source=1"
    } elseif ($EnablePhysicsIntervalRecord) {
        " physics_interval_record=900"
    } else {
        ""
    }
    Write-Output "FINAL_WRITER_NATURAL_ACTION_COMPOSITE_LIVE_PASSED frames=900 source=recorded_natural activation_frame=430 action_calls=1 dual_receipt=900 lifecycle_events=1 natural_cleanup=1$intervalPassText TracerPid=0"
    Write-Output "Unified report: $localUnified"
    Write-Output "Final-writer report: $localWriterReport"
    Write-Output "Natural-action report: $localActionReport"
    if ($physicsIntervalEnabled) {
        $intervalHash = (Get-FileHash -LiteralPath $localIntervalReceipt `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Output "Physics Interval $intervalMode receipt: $localIntervalReceipt"
        Write-Output "Physics Interval SHA256: $intervalHash"
    }
} finally {
    foreach ($marker in @($nalReady, $nalArm, $writerReady)) {
        & $AdbPath -s $Device shell "su -c 'rm -f $marker'" 2>$null | Out-Null
    }
    if (-not $success -and $externalReady) {
        & $AdbPath -s $Device shell "su -c 'rm -f $externalReady'" 2>$null | Out-Null
    }
    if ($intervalInstalled -and -not $intervalFinalized -and $intervalObject) {
        try {
            if ((Get-GamePid) -eq $gamePid -and
                (Get-StartTicks $gamePid) -eq $startTicks -and
                (Get-TracerPid $gamePid) -eq 0) {
                $rollbackOutput = "$remoteIntervalPrefix.rollback.a9pgtr2"
                $rollbackCommand = "$remoteIntervalController rollback $gamePid $startTicks $base $intervalObject $intervalMode $intervalLimit - $rollbackOutput $intervalAck"
                $rollbackText = (Invoke-AdbChecked @('-s', $Device, 'shell',
                    "su -c '$rollbackCommand'") 'rollback Physics Interval hook') -join "`n"
                if ($rollbackText -notmatch 'PHYSICS_INTERVAL_SHADOW_TRANSACTION action=4' -or
                    $rollbackText -notmatch 'game_writes=1') {
                    throw "Physics Interval rollback proof mismatch"
                }
                Write-Warning "Composite failed; Physics Interval vptr was restored before process cleanup"
            }
        } catch {
            Write-Warning "Physics Interval rollback could not be proven: $($_.Exception.Message)"
        }
    }
    if (-not $success -and $mutationStarted) {
        & $AdbPath -s $Device shell "am force-stop $package" 2>$null | Out-Null
        if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
        Write-Warning "Composite attempt failed after resume; exact fresh game process was force-stopped"
    }
}
