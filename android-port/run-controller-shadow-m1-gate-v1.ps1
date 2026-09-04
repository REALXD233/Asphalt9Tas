# Guarded M1 gate: register the natural-action callback, prove the complete
# runtime graph read-only, then replay exactly five authoritative frames.
# The default mode builds and validates local artifacts without touching ADB.

param(
    [ValidateSet("OfflineValidate", "PrepareFreshProcess", "ReadOnlyBasePreflight", "ExecuteFiveFrameGate")]
    [string]$Mode = "OfflineValidate",
    [ValidateRange(30000, 180000)][int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputDirectory = "",
    [switch]$AcknowledgeFreshProcessPreload,
    [switch]$AcknowledgeReadOnlyBasePreflight,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeThreeAutomaticEscapeTransitions,
    [switch]$AcknowledgeFiveFrameM1Writes,
    [switch]$AcknowledgeNoManualInputDuringGate,
    [switch]$AcknowledgeFailureForceStopsFreshProcess,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk,
    [switch]$ExecuteExactlyOneAttempt
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"

$buildScripts = @(
    (Join-Path $root "build-final-writer-replay-v1.ps1"),
    (Join-Path $root "build-natural-action-replay-payload-v1.ps1"),
    (Join-Path $root "build-controller-shadow-coordinator-v1.ps1"),
    (Join-Path $root "build-natural-action-external-replay-controller-v1.ps1"),
    (Join-Path $root "build-m1-three-payload-preload-v1.ps1"),
    (Join-Path $root "build-gameplay-input-controller-resolver-v1.ps1"),
    (Join-Path $root "build-controller-shadow-m1-preflight-v1.ps1"),
    (Join-Path $root "build-controller-shadow-m1-executor-v1.ps1"),
    (Join-Path $root "prepare-m1-five-frame-gate-v1.ps1")
)
$writerPayload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$naturalPayload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$controllerPayload = Join-Path $root "build\controller-shadow-coordinator-v1\liba9tas_controller_shadow_coordinator_v1_build_only.so"
$bundle = Join-Path $root "build\m1-three-payload-preload-v1\liba9tas_payload_bundle_natural_controller_v1.so"
$bootstrap = Join-Path $root "build\m1-three-payload-preload-v1\liba9tas_bootstrap_final_writer_m1_v1.so"
$actionController = Join-Path $root "build\natural-action-external-replay-controller-v1\natural_action_external_replay_controller_v1_review_only"
$preflight = Join-Path $root "build\controller-shadow-m1-preflight-v1\a9tas_controller_shadow_m1_preflight_v1_review_only"
$executor = Join-Path $root "build\controller-shadow-m1-executor-v1\a9tas_controller_shadow_m1_executor_v1_review_only"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_m1_three_payload_preload_v1.sh"
$recording = Join-Path $root "build\m1-five-frame-gate-v1\a9tas_m1_gate_5f.a9utk1"
$target = Join-Path $root "build\m1-five-frame-gate-v1\a9tas_m1_gate_5f.a9fwt1"
$m1Parser = Join-Path $root "tools\parse_m1_executor_report_v1.py"
$actionValidator = Join-Path $root "tools\validate_natural_action_external_replay_report_v1.py"
$policy = Join-Path $root "tools\test_run_controller_shadow_m1_gate_policy_v1.py"
$receipt = Join-Path $root "build\controller-shadow-m1-gate-v1\prepared-process-v1.json"

$pins = @{
    $writerPayload = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
    $naturalPayload = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
    $controllerPayload = "636d0759cb8ccd4830c7de2b82b6a45b148f98ce4656ada53045e54df3c2f7de"
    $bundle = "c0c20904ec303b8a68dae3f4b84327b985128b05eb09b842a77f81061e33ee15"
    $bootstrap = "2face9181d226e6592d31ce168814a586077b2f635e5a4aa3040852d0f74a8ef"
    $actionController = "7a1659ef56bdd7cad3591ad06ab533b1fa72f31adc1598fec1b8e4c7958a2138"
    $preflight = "c2f1043cc280ed8397d65f6af80635d590434ddc88ab5078dfd78abc0fa497e2"
    $executor = "b1c913b28ff7e77c4245419ef1c540270cbff94113513ae5749e23950dfdc795"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "02c284730feef7c738b0b055158bf01e4e371a532a138f00460ce3864072ab67"
    $recording = "3d29bebb3d6ef8f2398a905a08073f15aec72e46588139b16e2525744274ddab"
    $target = "9e60c88c97e25902ba3de8c280b314f40dfc962ce095bec29d3965cf46672005"
    $m1Parser = "f9c3257c6a8c3768e827408fb8f751ad4c918a9c306964e835e0e1abdee789f9"
    $actionValidator = "c138f75d46d51938319988731a632ab218c31e289289cf89fdb460fd116b7683"
}

$remoteWriterPayload = "/data/local/tmp/liba9tas_final_writer_replay_v1_build_only.so"
$remoteNaturalPayload = "/data/local/tmp/liba9tas_natural_action_replay_v1_review_only.so"
$remoteControllerPayload = "/data/local/tmp/liba9tas_controller_shadow_coordinator_v1_build_only.so"
$remoteBundle = "/data/local/tmp/liba9tas_payload_bundle_natural_controller_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_m1_v1.so"
$remoteActionController = "/data/local/tmp/a9tas_natural_action_external_replay_m1_v1"
$remotePreflight = "/data/local/tmp/a9tas_controller_shadow_m1_preflight_v1"
$remoteExecutor = "/data/local/tmp/a9tas_controller_shadow_m1_executor_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_m1_v1"
$remoteHelper = "/data/local/tmp/run_m1_three_payload_preload_v1.sh"
$actionAck = "I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1"
$basePreflightAck = "I_ACCEPT_M1_BASE_READ_ONLY_PREFLIGHT_V1"
$preflightAck = "I_ACCEPT_M1_READ_ONLY_PREFLIGHT_V1"
$executorAck = "I_ACCEPT_M1_EXECUTOR_REVIEW_V1"

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments 2>&1) | Out-String).Trim()
}
function Invoke-AdbChecked([string[]]$Arguments, [string]$Label) {
    $result = & $AdbPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($result -join ' ')" }
    return (($result | Out-String).Trim())
}
function Invoke-RemoteChecked([string]$Command, [string]$Label) {
    $result = & $AdbPath -s $Device shell "su -c '$Command'" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($result -join ' ')" }
    return (($result | Out-String).Trim())
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
function Assert-LocalPins {
    foreach ($path in $pins.Keys) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing M1 input: $path"
        }
        if ((Get-Sha $path) -ne $pins[$path]) { throw "M1 pin mismatch: $path" }
    }
}

foreach ($path in @($buildScripts + @($policy))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing M1 build/policy input: $path"
    }
}
python -B $policy $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "M1 runner policy failed" }
if ($Mode -eq 'OfflineValidate') {
    foreach ($script in $buildScripts) {
        & $script | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "M1 offline build failed: $script" }
    }
    Assert-LocalPins
    Write-Output "M1_GATE_OFFLINE passed=1 frames=5 lifecycle_gate=2_to_3 frozen_ready_gate=1 fixed_delta_writes=5 controller_payload=1 final_writer=1 natural_action=1 deployed=0 device_access=0"
    return
}

# The single-address live path is retired after Attempt 4 proved that repeated
# accumulator accesses do not identify authoritative game-frame boundaries.
# Keep OfflineValidate available for historical regression, but do not permit
# this runner to prepare, inspect, or execute another live process.  Its
# successor must use the proven delta -> C98 -> C9C -> world-commit topology
# and receive a separately reviewed runner and authorization contract.
throw "Single-address M1 live modes are retired; use the phase-paced successor after review"

Assert-LocalPins

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}

if ($Mode -eq 'PrepareFreshProcess') {
    if (-not $AcknowledgeFreshProcessPreload) { throw "Must acknowledge fresh-process M1 preload" }
    if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") 'force-stop old game' | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    foreach ($pair in @(
        @($writerPayload, $remoteWriterPayload),
        @($naturalPayload, $remoteNaturalPayload),
        @($controllerPayload, $remoteControllerPayload),
        @($bundle, $remoteBundle),
        @($bootstrap, $remoteBootstrap),
        @($injector, $remoteInjector),
        @($helper, $remoteHelper)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push M1 preload artifact' | Out-Null
        Assert-RemoteHash $pair[1] $pins[$pair[0]]
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remoteWriterPayload $remoteNaturalPayload $remoteControllerPayload $remoteBundle $remoteBootstrap; chmod 700 $remoteInjector $remoteHelper'") 'chmod M1 preload artifacts' | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $receipt) -Force | Out-Null
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
        throw "M1 preload timed out"
    }
    $preloadText = (Get-Content -Raw $preloadOut) + "`n" + (Get-Content -Raw $preloadErr)
    if ($preload.ExitCode -ne 0 -or $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "M1 preload did not reach armed stage: $preloadText"
    }
    $gamePid = Get-GamePid
    if ($gamePid -le 0) { throw "Fresh game PID unavailable" }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $mapped = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
        if ($maps.Contains($remoteWriterPayload) -and
            $maps.Contains($remoteNaturalPayload) -and
            $maps.Contains($remoteControllerPayload) -and
            $maps.Contains($remoteBundle) -and $maps.Contains($remoteBootstrap)) {
            $mapped = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped -or (Get-TracerPid $gamePid) -ne 0) {
        throw "M1 payload mapping or tracer proof failed"
    }
    $startTicks = Get-StartTicks $gamePid
    [ordered]@{
        version=1; device=$Device; pid=$gamePid; start_ticks=$startTicks;
        writer_payload_sha256=$pins[$writerPayload];
        natural_payload_sha256=$pins[$naturalPayload];
        controller_payload_sha256=$pins[$controllerPayload];
        bundle_sha256=$pins[$bundle]; bootstrap_sha256=$pins[$bootstrap]
    } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    Write-Output "M1_GATE_PREPARE_PASSED pid=$gamePid start_ticks=$startTicks payloads=3 TracerPid=0"
    return
}

if ($Mode -eq 'ReadOnlyBasePreflight') {
    foreach ($gate in @(
        @($ExecuteExactlyOneAttempt, 'exactly one read-only attempt'),
        @($AcknowledgeReadOnlyBasePreflight, 'read-only base preflight'),
        @($AcknowledgeAncientRuinsZl1Countdown3Paused, 'Ancient Ruins + ZL1 countdown-3 paused')
    )) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
} else {
    foreach ($gate in @(
        @($ExecuteExactlyOneAttempt, 'exactly one attempt'),
        @($AcknowledgeAncientRuinsZl1Countdown3Paused, 'Ancient Ruins + ZL1 countdown-3 paused'),
        @($AcknowledgeThreeAutomaticEscapeTransitions, 'automatic resume, re-pause, and final resume ESC transitions'),
        @($AcknowledgeFiveFrameM1Writes, 'five fixed-delta frames plus payload-owned control, action, and conditional writer work'),
        @($AcknowledgeNoManualInputDuringGate, 'no manual input during the gate'),
        @($AcknowledgeFailureForceStopsFreshProcess, 'fresh process force-stop on uncertain failure'),
        @($AcknowledgePtraceStallRollbackAndCrashRisk, 'ptrace stall, rollback, and crash risk')
    )) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
}
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared-process receipt missing" }

$prepared = Get-Content -Raw $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
$startTicks = if ($gamePid -gt 0) { Get-StartTicks $gamePid } else { '' }
if ($prepared.version -ne 1 -or $prepared.device -ne $Device -or
    [int]$prepared.pid -ne $gamePid -or
    [string]$prepared.start_ticks -ne $startTicks -or
    $prepared.writer_payload_sha256 -ne $pins[$writerPayload] -or
    $prepared.natural_payload_sha256 -ne $pins[$naturalPayload] -or
    $prepared.controller_payload_sha256 -ne $pins[$controllerPayload] -or
    $prepared.bundle_sha256 -ne $pins[$bundle] -or
    $prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or
    (Get-TracerPid $gamePid) -ne 0) {
    throw "Prepared M1 process identity mismatch"
}
$maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
foreach ($mapping in @($remoteWriterPayload, $remoteNaturalPayload,
                       $remoteControllerPayload, $remoteBundle, $remoteBootstrap)) {
    if (-not $maps.Contains($mapping)) { throw "Prepared M1 mapping missing: $mapping" }
}
$base = Get-GameBaseHex $gamePid
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
if ($OutputDirectory -eq '') { $OutputDirectory = Join-Path $root 'evidence' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$remoteRecording = "/data/local/tmp/a9tas_m1_${gamePid}_$stamp.a9utk1"
$remoteTarget = "/data/local/tmp/a9tas_m1_${gamePid}_$stamp.a9fwt1"
$remoteM1Report = "/data/local/tmp/a9tas_m1_${gamePid}_$stamp.a9m1ex1"
$remoteActionReport = "/data/local/tmp/a9tas_m1_${gamePid}_$stamp.a9nar6"
$nalReady = "/data/local/tmp/a9tas_m1_nal_ready_${gamePid}_$stamp"
$nalArm = "/data/local/tmp/a9tas_m1_nal_arm_${gamePid}_$stamp"
$externalReady = "/data/local/tmp/a9tas_m1_external_${gamePid}_$stamp"
$m1Ready = "/data/local/tmp/a9tas_m1_ready_${gamePid}_$stamp"
$escDispatched = "/data/local/tmp/a9tas_m1_esc_dispatched_${gamePid}_$stamp"
$localM1Report = Join-Path $OutputDirectory "a9tas_m1_gate_5f_$stamp.a9m1ex1"
$localActionReport = Join-Path $OutputDirectory "a9tas_m1_gate_5f_$stamp.a9nar6"

if ($Mode -eq 'ReadOnlyBasePreflight') {
    try {
        foreach ($pair in @(
            @($preflight, $remotePreflight, $pins[$preflight]),
            @($recording, $remoteRecording, $pins[$recording]),
            @($target, $remoteTarget, $pins[$target])
        )) {
            Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push read-only preflight artifact' | Out-Null
            Assert-RemoteHash $pair[1] $pair[2]
        }
        Invoke-AdbChecked @('-s', $Device, 'shell',
            "su -c 'chmod 700 $remotePreflight'") 'chmod read-only preflight' | Out-Null
        $basePreflightText = Invoke-RemoteChecked "$remotePreflight $gamePid $base $startTicks $remoteRecording $remoteTarget $basePreflightAck" 'M1 base read-only preflight'
        if ($basePreflightText -notmatch '^M1_BASE_PREFLIGHT_OK ' -or
            $basePreflightText -notmatch 'frames=5 ' -or
            $basePreflightText -notmatch 'lifecycle_state=2 ' -or
            $basePreflightText -notmatch 'natural_state=resolved target_blob_bound=1 attach_attempts=0 process_writes=0 device_mutations=0$') {
            throw "M1 base read-only proof mismatch: $basePreflightText"
        }
        $basePreflightText | Write-Host
        Write-Output "M1_BASE_PREFLIGHT_PASSED pid=$gamePid start_ticks=$startTicks frames=5 process_writes=0 attach_attempts=0 automatic_esc=0"
    }
    finally {
        foreach ($path in @($remoteRecording, $remoteTarget)) {
            try { Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $path'") 'remove read-only preflight temporary' | Out-Null } catch {}
        }
    }
    return
}

$mutationStarted = $false
$success = $false
$actionHandle = $null
$executorHandle = $null
$actionResultConsumed = $false
$executorResultConsumed = $false
try {
    foreach ($pair in @(
        @($actionController, $remoteActionController, $pins[$actionController]),
        @($preflight, $remotePreflight, $pins[$preflight]),
        @($executor, $remoteExecutor, $pins[$executor]),
        @($recording, $remoteRecording, $pins[$recording]),
        @($target, $remoteTarget, $pins[$target])
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push M1 gate artifact' | Out-Null
        Assert-RemoteHash $pair[1] $pair[2]
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 700 $remoteActionController $remotePreflight $remoteExecutor'") 'chmod M1 gate candidates' | Out-Null

    $basePreflightText = Invoke-RemoteChecked "$remotePreflight $gamePid $base $startTicks $remoteRecording $remoteTarget $basePreflightAck" 'M1 base read-only preflight'
    if ($basePreflightText -notmatch '^M1_BASE_PREFLIGHT_OK ' -or
        $basePreflightText -notmatch 'frames=5 ' -or
        $basePreflightText -notmatch 'lifecycle_state=2 ' -or
        $basePreflightText -notmatch 'natural_state=resolved target_blob_bound=1 attach_attempts=0 process_writes=0 device_mutations=0$') {
        throw "M1 base read-only proof mismatch: $basePreflightText"
    }
    $basePreflightText | Write-Host

    $actionCommand = "$remoteActionController $gamePid $startTicks $base $TimeoutMs $remoteActionReport $nalReady $nalArm $externalReady 5 $actionAck"
    $actionHandle = Start-Remote $actionCommand
    if (-not (Wait-RemoteFile $nalReady 7000)) { throw "Natural-action registration did not reach READY_NO_ATTACH" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $nalReady'")
    if ($readyText -notmatch '^NAL_READY_NO_ATTACH ' -or
        $readyText -notmatch 'paused_zero_samples=8 target_threads_attached=0 game_writes=0') {
        throw "Natural-action READY proof mismatch"
    }
    $mutationStarted = $true
    # Paused outer cycles also contain positive accumulator writes.  Dispatch
    # ESC first and give the game a bounded window to consume it before
    # publishing ARM; otherwise the read-only positive-delta prearm can attach
    # to a still-paused cycle and wait forever for a physics callback.
    Invoke-AdbChecked @('-s', $Device, 'shell', 'input keyevent 111') 'resume natural callback registration' | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
        (Get-TracerPid $gamePid) -ne 0) {
        throw "Natural registration ESC settle identity mismatch"
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'echo NAL_ARM $gamePid $startTicks > $nalArm'") 'arm natural callback after ESC settle' | Out-Null
    $registrationDeadline = [DateTime]::UtcNow.AddMilliseconds(30000)
    $registrationState = 'timeout'
    while ([DateTime]::UtcNow -lt $registrationDeadline) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $externalReady; echo `$?'")) -eq '0') {
            $registrationState = 'ready'
            break
        }
        if ($actionHandle.Process.HasExited) {
            $registrationState = 'controller_exit'
            break
        }
        Start-Sleep -Milliseconds 25
    }
    if ($registrationState -ne 'ready') {
        if ($actionHandle.Process.HasExited) {
            $registrationResult = Read-RemoteResult $actionHandle
            $actionResultConsumed = $true
            if ($registrationResult.Stdout) { $registrationResult.Stdout.TrimEnd() | Write-Warning }
            if ($registrationResult.Stderr) { $registrationResult.Stderr.TrimEnd() | Write-Warning }
            throw "Persistent natural callback registration failed state=$registrationState exit=$($registrationResult.ExitCode)"
        }
        throw "Persistent natural callback registration failed state=$registrationState"
    }
    if ((Get-TracerPid $gamePid) -ne 0) { throw "Natural controller did not detach before M1 handoff" }
    $externalText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $externalReady'")
    if ($externalText -notmatch '^NAL_EXTERNAL_REPLAY_READY ' -or
        $externalText -notmatch 'frames=5 tracer=0 published=0 completed=0$') {
        throw "Empty natural mailbox proof mismatch"
    }
    Invoke-AdbChecked @('-s', $Device, 'shell', 'input keyevent 111') 're-pause after callback registration' | Out-Null
    Start-Sleep -Milliseconds 250

    $preflightText = Invoke-RemoteChecked "$remotePreflight $gamePid $base $startTicks $remoteRecording $remoteTarget $preflightAck" 'M1 read-only preflight'
    if ($preflightText -notmatch '^M1_PREFLIGHT_OK ' -or
        $preflightText -notmatch 'frames=5 ' -or
        $preflightText -notmatch 'lifecycle_state=2 ' -or
        $preflightText -notmatch 'attach_attempts=0 process_writes=0 device_mutations=0$') {
        throw "M1 read-only preflight proof mismatch: $preflightText"
    }
    $preflightText | Write-Host

    $executorCommand = "$remoteExecutor $gamePid $base $startTicks $TimeoutMs $remoteRecording $remoteTarget $remoteM1Report $m1Ready $executorAck"
    $executorHandle = Start-Remote $executorCommand
    if (-not (Wait-RemoteFile $m1Ready 15000)) { throw "M1 executor did not reach frozen READY" }
    $m1ReadyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $m1Ready'")
    if ($m1ReadyText -notmatch '^M1_READY_ARMED ' -or
        $m1ReadyText -notmatch 'state=2 ' -or
        $m1ReadyText -notmatch 'frames=5 writer_installed=1 controller_installed=1 ' -or
        $m1ReadyText -notmatch 'all_target_threads_frozen=1 delta_writes=0 host_resume_gate=marker_removal controller_pid=(\d+)$') {
        throw "M1 frozen READY proof mismatch: $m1ReadyText"
    }
    $controllerPid = [int]$matches[1]
    if ((Get-TracerPid $gamePid) -ne $controllerPid) { throw "M1 tracer identity mismatch" }

    $escHandle = Start-Remote "input keyevent 111 & echo M1_ESC_DISPATCHED $gamePid $startTicks > $escDispatched; wait"
    if (-not (Wait-RemoteFile $escDispatched 3000)) { throw "Final resume ESC was not dispatched" }
    $escProof = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $escDispatched'")
    if ($escProof -ne "M1_ESC_DISPATCHED $gamePid $startTicks") { throw "Final resume ESC dispatch proof mismatch" }
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $m1Ready'") 'release M1 frozen gate' | Out-Null
    if (-not $escHandle.Process.WaitForExit(10000)) { throw "Final resume ESC failed" }
    $escResult = Read-RemoteResult $escHandle
    if ($escResult.ExitCode -ne 0) { throw "Final resume ESC failed: $($escResult.Stderr)" }
    if (-not $executorHandle.Process.WaitForExit($TimeoutMs + 15000)) { throw "M1 executor timed out" }
    $executorResult = Read-RemoteResult $executorHandle
    $executorResultConsumed = $true
    if ($executorResult.Stdout) { $executorResult.Stdout.TrimEnd() | Write-Host }
    if ($executorResult.Stderr) { $executorResult.Stderr.TrimEnd() | Write-Warning }
    # The executor writes its report on both success and fail-closed exits.
    # Preserve that report before throwing so a rejected three-way completion
    # gate never requires a second live attempt merely for diagnostics.
    $m1ReportPulled = $false
    try {
        Invoke-AdbChecked @('-s', $Device, 'pull', $remoteM1Report, $localM1Report) 'pull M1 report' | Out-Null
        $m1ReportPulled = $true
    } catch {
        Write-Warning "M1 report could not be preserved: $($_.Exception.Message)"
    }
    if ($executorResult.ExitCode -ne 0 -or
        $executorResult.Stdout -notmatch 'M1_EXECUTOR_DONE success=1 frames=5 ' -or
        $executorResult.Stdout -notmatch 'delta_writes=5 ') {
        $suffix = if ($m1ReportPulled) { " local_report=$localM1Report" } else { " local_report=unavailable" }
        throw "M1 executor failed closed.$suffix"
    }
    if ((Get-TracerPid $gamePid) -ne 0) { throw "M1 executor did not detach" }

    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $externalReady'") 'release natural cleanup' | Out-Null
    if (-not $actionHandle.Process.WaitForExit($TimeoutMs + 15000)) { throw "Natural callback cleanup timed out" }
    $actionResult = Read-RemoteResult $actionHandle
    $actionResultConsumed = $true
    if ($actionResult.Stdout) { $actionResult.Stdout.TrimEnd() | Write-Host }
    if ($actionResult.Stderr) { $actionResult.Stderr.TrimEnd() | Write-Warning }
    if ($actionResult.ExitCode -ne 0 -or $actionResult.Stdout -notmatch 'NAL_DONE success=1 ') {
        throw "Natural callback cleanup failed"
    }
    if ((Get-TracerPid $gamePid) -ne 0) { throw "Final tracer state is not clean" }

    if (-not $m1ReportPulled) {
        Invoke-AdbChecked @('-s', $Device, 'pull', $remoteM1Report, $localM1Report) 'pull M1 report' | Out-Null
    }
    Invoke-AdbChecked @('-s', $Device, 'pull', $remoteActionReport, $localActionReport) 'pull natural report' | Out-Null
    python -B $m1Parser $localM1Report $recording $controllerPayload --frames 5
    if ($LASTEXITCODE -ne 0) { throw "M1 report validation failed" }
    python -B $actionValidator $localActionReport $recording
    if ($LASTEXITCODE -ne 0) { throw "Natural action report validation failed" }
    $success = $true
    Write-Output "M1_GATE_PASSED pid=$gamePid start_ticks=$startTicks frames=5 delta_writes=5 lifecycle=2_to_3 receipts=controller+writer+action cleanup=controller+writer+natural+detach"
    Write-Output "m1_report=$localM1Report"
    Write-Output "natural_report=$localActionReport"
}
finally {
    foreach ($path in @($nalReady, $nalArm, $externalReady, $m1Ready,
                         $escDispatched,
                         $remoteRecording, $remoteTarget)) {
        try { Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $path'") 'remove M1 temporary' | Out-Null } catch {}
    }
    if (-not $success -and $mutationStarted) {
        try { Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") 'force-stop uncertain M1 process' | Out-Null } catch {}
        if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
    }
    if (-not $success) {
        foreach ($entry in @(
            @($executorHandle, 'M1 executor', $executorResultConsumed),
            @($actionHandle, 'natural callback controller', $actionResultConsumed)
        )) {
            $handle = $entry[0]
            if ($null -eq $handle -or [bool]$entry[2]) { continue }
            try {
                if (-not $handle.Process.HasExited) { $null = $handle.Process.WaitForExit(5000) }
                if ($handle.Process.HasExited) {
                    $failedResult = Read-RemoteResult $handle
                    if ($failedResult.Stdout) { $failedResult.Stdout.TrimEnd() | Write-Warning }
                    if ($failedResult.Stderr) { $failedResult.Stderr.TrimEnd() | Write-Warning }
                }
            } catch {
                Write-Warning "$($entry[1]) failure diagnostics unavailable: $($_.Exception.Message)"
            }
        }
        try {
            if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -f $remoteM1Report; echo `$?'")) -eq '0') {
                Invoke-AdbChecked @('-s', $Device, 'pull', $remoteM1Report, $localM1Report) 'preserve failed M1 executor report' | Out-Null
                Write-Warning "Preserved failed report: $localM1Report"
            }
        } catch {}
        try {
            if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -f $remoteActionReport; echo `$?'")) -eq '0') {
                Invoke-AdbChecked @('-s', $Device, 'pull', $remoteActionReport, $localActionReport) 'preserve failed natural report' | Out-Null
                Write-Warning "Preserved failed report: $localActionReport"
            }
        } catch {}
    }
    foreach ($path in @($remoteM1Report, $remoteActionReport)) {
        try { Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $path'") 'remove M1 report temporary' | Out-Null } catch {}
    }
}
